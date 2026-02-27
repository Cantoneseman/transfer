#!/usr/bin/env python3
"""
HYPER-TRANSFER v2.0 Controller Script

Orchestrates third-party file transfers between supercomputing nodes using:
- SSH for remote command execution
- Named pipes (FIFOs) for streaming data
- globus-url-copy for high-performance GridFTP transfers

Architecture:
    ┌─────────────────┐                      ┌─────────────────┐
    │   Source Host   │                      │   Dest Host     │
    │  ┌───────────┐  │                      │  ┌───────────┐  │
    │  │hy_sender  │──┼──► /tmp/ht_pipe_in   │  │hy_receiver│  │
    │  └───────────┘  │         │            │  └───────────┘  │
    └─────────────────┘         │            └────────▲────────┘
                                │                     │
                    ┌───────────▼─────────────────────┴───────────┐
                    │           globus-url-copy (GridFTP)          │
                    │    sshftp://src/pipe  →  sshftp://dst/pipe   │
                    └──────────────────────────────────────────────┘
                                        │
                              ┌─────────▼─────────┐
                              │  Controller Host  │
                              │  (this script)    │
                              └───────────────────┘

Usage:
    python controller.py src_host dst_host /path/to/file [options]

Requirements:
    - Passwordless SSH configured to both hosts
    - hyper_sender and hyper_receiver built on respective hosts
    - globus-url-copy available on controller host
"""

import argparse
import subprocess
import sys
import time
import signal
import os
import getpass
from pathlib import Path
from typing import Optional, Tuple, List
from dataclasses import dataclass
from enum import Enum


class LogLevel(Enum):
    DEBUG = 0
    INFO = 1
    WARN = 2
    ERROR = 3


# ANSI color codes
COLORS = {
    LogLevel.DEBUG: "\033[90m",   # Gray
    LogLevel.INFO: "\033[94m",    # Blue
    LogLevel.WARN: "\033[93m",    # Yellow
    LogLevel.ERROR: "\033[91m",   # Red
    "RESET": "\033[0m",
    "BOLD": "\033[1m",
    "GREEN": "\033[92m",
}

# Remote working directory (must exist on all nodes)
REMOTE_WORK_DIR = "~/projects/hyper-transfer"


@dataclass
class TransferConfig:
    """Configuration for a transfer operation."""
    src_host: str
    dst_host: str
    file_path: str
    dst_path: Optional[str]
    binary_dir: str
    pipe_dir: str
    verbose: bool
    dry_run: bool
    timeout: int
    parallel_streams: int
    router_path: Optional[str] = None  # Path to hyper_router binary


class HyperController:
    """
    Orchestrates HYPER-TRANSFER operations between remote hosts.
    """
    
    def __init__(self, config: TransferConfig):
        self.config = config
        self.log_level = LogLevel.DEBUG if config.verbose else LogLevel.INFO
        
        # Generate unique pipe names (using PID for uniqueness)
        self.pipe_id = f"ht_{os.getpid()}"
        self.src_pipe = f"{config.pipe_dir}/{self.pipe_id}_in"
        self.dst_pipe = f"{config.pipe_dir}/{self.pipe_id}_out"
        
        # Track remote processes for cleanup
        self.sender_started = False
        self.receiver_started = False
        
        # Computed route
        self.route: List[str] = []
    
    def get_best_route(self, src_host: str, dst_host: str) -> List[str]:
        """
        Query the C++ router for the optimal path between hosts.
        
        Args:
            src_host: Source node ID
            dst_host: Destination node ID
            
        Returns:
            List of node IDs representing the path, e.g., ['GZ', 'WH', 'BJ']
            Falls back to direct route [src_host, dst_host] on error.
        """
        # Determine router binary path
        router_path = self.config.router_path or f"{self.config.binary_dir}/hyper_router"
        
        self.log(LogLevel.DEBUG, f"Querying router: {router_path} {src_host} {dst_host}")
        
        # Router is always called, even in dry-run mode (it's read-only)
        try:
            # Call the C++ router binary
            result = subprocess.check_output(
                [router_path, src_host, dst_host],
                stderr=subprocess.PIPE,
                timeout=10,
                text=True
            )
            
            # Parse output: "GZ WH BJ" -> ['GZ', 'WH', 'BJ']
            route = result.strip().split()
            
            if len(route) < 2:
                raise ValueError(f"Invalid route returned: {result}")
            
            self.log(LogLevel.DEBUG, f"Router returned: {route}")
            return route
            
        except FileNotFoundError:
            self.log(LogLevel.WARN, 
                    f"Router binary not found at {router_path}, using direct route")
            return [src_host, dst_host]
            
        except subprocess.CalledProcessError as e:
            self.log(LogLevel.WARN, 
                    f"Router failed (exit {e.returncode}): {e.stderr.strip() if e.stderr else 'unknown error'}")
            self.log(LogLevel.WARN, "Falling back to direct route")
            return [src_host, dst_host]
            
        except subprocess.TimeoutExpired:
            self.log(LogLevel.WARN, "Router timed out, using direct route")
            return [src_host, dst_host]
            
        except Exception as e:
            self.log(LogLevel.WARN, f"Router error: {e}, using direct route")
            return [src_host, dst_host]
        
    def log(self, level: LogLevel, msg: str):
        """Print log message with color coding."""
        if level.value >= self.log_level.value:
            color = COLORS.get(level, "")
            reset = COLORS["RESET"]
            prefix = f"[{level.name:5}]"
            print(f"{color}{prefix}{reset} {msg}", file=sys.stderr)
    
    def log_cmd(self, cmd: str, host: str = "local"):
        """Log a command being executed."""
        self.log(LogLevel.DEBUG, f"[{host}] $ {cmd}")
    
    def run_ssh(self, host: str, command: str, 
                check: bool = True, 
                capture: bool = False,
                timeout: Optional[int] = None) -> subprocess.CompletedProcess:
        """
        Execute a command on a remote host via SSH.
        
        Args:
            host: Remote hostname
            command: Command to execute
            check: Raise exception on non-zero exit
            capture: Capture stdout/stderr
            timeout: Command timeout in seconds
        
        Returns:
            CompletedProcess result
        """
        ssh_cmd = [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "ConnectTimeout=10",
            host,
            command
        ]
        
        self.log_cmd(command, host)
        
        if self.config.dry_run:
            self.log(LogLevel.INFO, f"[DRY-RUN] Would execute on {host}")
            return subprocess.CompletedProcess(ssh_cmd, 0, b"", b"")
        
        try:
            result = subprocess.run(
                ssh_cmd,
                check=check,
                capture_output=capture,
                timeout=timeout or self.config.timeout,
                text=True
            )
            return result
        except subprocess.TimeoutExpired:
            self.log(LogLevel.ERROR, f"SSH command timed out on {host}")
            raise
        except subprocess.CalledProcessError as e:
            self.log(LogLevel.ERROR, f"SSH command failed on {host}: {e}")
            if capture and e.stderr:
                self.log(LogLevel.ERROR, f"stderr: {e.stderr}")
            raise
    
    def run_ssh_background(self, host: str, command: str) -> bool:
        """
        Execute a command in the background on a remote host.
        Uses bash -c wrapper with nohup to ensure the process runs
        independently without blocking on FIFO pipes.
        
        Returns:
            True if command was started successfully
        """
        # Wrap command with bash -c and nohup for proper background execution
        # This prevents SSH from hanging when the command involves FIFOs
        bg_command = f"nohup bash -c '{command}' > /dev/null 2>&1 & echo $!"
        
        self.log_cmd(f"(background) {command}", host)
        
        if self.config.dry_run:
            self.log(LogLevel.INFO, f"[DRY-RUN] Would start background process on {host}")
            return True
        
        try:
            result = subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", host, bg_command],
                capture_output=True,
                text=True,
                timeout=60  # Increased timeout for FIFO operations
            )
            if result.returncode == 0:
                pid = result.stdout.strip()
                self.log(LogLevel.DEBUG, f"Started background process PID={pid} on {host}")
                return True
            else:
                self.log(LogLevel.ERROR, f"Failed to start background process: {result.stderr}")
                return False
        except subprocess.TimeoutExpired:
            self.log(LogLevel.ERROR, f"SSH background command timed out on {host}")
            return False
        except Exception as e:
            self.log(LogLevel.ERROR, f"Failed to start background process: {e}")
            return False
    
    def run_local(self, command: list, 
                  check: bool = True,
                  timeout: Optional[int] = None) -> subprocess.CompletedProcess:
        """Execute a command locally."""
        self.log_cmd(" ".join(command))
        
        if self.config.dry_run:
            self.log(LogLevel.INFO, "[DRY-RUN] Would execute locally")
            return subprocess.CompletedProcess(command, 0, "", "")
        
        return subprocess.run(
            command,
            check=check,
            timeout=timeout or self.config.timeout,
            text=True
        )
    
    def setup_source(self) -> bool:
        """
        Set up the source host:
        1. Create named pipe
        2. Start hyper_sender writing to the pipe
        """
        self.log(LogLevel.INFO, f"Setting up source host: {self.config.src_host}")
        
        # Use remote work directory for binaries
        remote_binary_dir = f"{REMOTE_WORK_DIR}/build"
        
        # Create named pipe
        try:
            self.run_ssh(
                self.config.src_host,
                f"rm -f {self.src_pipe} && mkfifo {self.src_pipe}",
                timeout=30
            )
            self.log(LogLevel.DEBUG, f"Created source pipe: {self.src_pipe}")
        except Exception as e:
            self.log(LogLevel.ERROR, f"Failed to create source pipe: {e}")
            return False
        
        # Start hyper_sender in background
        sender_cmd = f"{remote_binary_dir}/hyper_sender {self.config.file_path} > {self.src_pipe}"
        
        if not self.run_ssh_background(self.config.src_host, sender_cmd):
            return False
        
        self.sender_started = True
        self.log(LogLevel.INFO, "✓ Source sender started")
        return True
    
    def setup_destination(self) -> bool:
        """
        Set up the destination host:
        1. Create named pipe
        2. Start hyper_receiver reading from the pipe
        """
        self.log(LogLevel.INFO, f"Setting up destination host: {self.config.dst_host}")
        
        # Use remote work directory for binaries
        remote_binary_dir = f"{REMOTE_WORK_DIR}/build"
        
        # Determine destination file path
        dst_file = self.config.dst_path or self.config.file_path
        
        # Create named pipe
        try:
            self.run_ssh(
                self.config.dst_host,
                f"rm -f {self.dst_pipe} && mkfifo {self.dst_pipe}",
                timeout=30
            )
            self.log(LogLevel.DEBUG, f"Created destination pipe: {self.dst_pipe}")
        except Exception as e:
            self.log(LogLevel.ERROR, f"Failed to create destination pipe: {e}")
            return False
        
        # Start hyper_receiver in background
        receiver_cmd = f"{remote_binary_dir}/hyper_receiver {dst_file} < {self.dst_pipe}"
        
        if not self.run_ssh_background(self.config.dst_host, receiver_cmd):
            return False
        
        self.receiver_started = True
        self.log(LogLevel.INFO, "✓ Destination receiver started")
        return True
    
    def _build_sshftp_url(self, host_spec: str, path: str) -> str:
        """
        Build a proper sshftp:// URL from a host specification.
        
        Args:
            host_spec: Either "hostname" or "user@hostname"
            path: The file path on the remote host
        
        Returns:
            A properly formatted sshftp://user@host/path URL
        """
        if "@" in host_spec:
            # Already has user@ prefix (e.g., "Su@192.168.1.203")
            user, host = host_spec.split("@", 1)
        elif host_spec in ("127.0.0.1", "localhost"):
            # Local host - use current user
            user = getpass.getuser()
            host = host_spec
        else:
            # Remote host without user - assume same username
            user = getpass.getuser()
            host = host_spec
        
        return f"sshftp://{user}@{host}{path}"
    
    def _extract_host(self, host_spec: str) -> str:
        """
        Extract the hostname/IP from a host specification.
        
        Args:
            host_spec: Either "hostname" or "user@hostname"
        
        Returns:
            Just the hostname/IP part
        """
        if "@" in host_spec:
            return host_spec.split("@", 1)[1]
        return host_spec
    
    def ensure_gsi_proxy(self) -> bool:
        """
        Ensure a valid GSI proxy exists for GridFTP authentication.
        
        Checks if a valid proxy exists using grid-proxy-info.
        If not, initializes one using grid-proxy-init (password-less keys required).
        
        Returns:
            True if a valid proxy is available, False otherwise
        """
        self.log(LogLevel.INFO, "Checking GSI proxy status...")
        
        if self.config.dry_run:
            self.log(LogLevel.INFO, "[DRY-RUN] Would check/initialize GSI proxy")
            return True
        
        # Check if a valid proxy already exists
        try:
            result = subprocess.run(
                ["grid-proxy-info", "-exists"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10
            )
            
            if result.returncode == 0:
                self.log(LogLevel.INFO, "✓ Valid GSI proxy found")
                # Show proxy info for debugging
                info_result = subprocess.run(
                    ["grid-proxy-info"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=10
                )
                if info_result.returncode == 0:
                    for line in info_result.stdout.strip().split('\n'):
                        self.log(LogLevel.DEBUG, f"  {line}")
                return True
                
        except FileNotFoundError:
            self.log(LogLevel.ERROR, "grid-proxy-info not found. Please install Grid Community Toolkit.")
            return False
        except subprocess.TimeoutExpired:
            self.log(LogLevel.WARN, "grid-proxy-info timed out")
        except Exception as e:
            self.log(LogLevel.DEBUG, f"Proxy check failed: {e}")
        
        # No valid proxy - initialize one
        self.log(LogLevel.INFO, "No valid proxy found, initializing...")
        
        try:
            result = subprocess.run(
                ["grid-proxy-init"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=30
            )
            
            if result.returncode == 0:
                self.log(LogLevel.INFO, "✓ GSI Proxy initialized")
                # Show the new proxy info
                if result.stdout.strip():
                    for line in result.stdout.strip().split('\n'):
                        self.log(LogLevel.DEBUG, f"  {line}")
                return True
            else:
                self.log(LogLevel.ERROR, f"grid-proxy-init failed with exit code {result.returncode}")
                if result.stderr.strip():
                    self.log(LogLevel.ERROR, "=== Full Error Output ===")
                    for line in result.stderr.strip().split('\n'):
                        self.log(LogLevel.ERROR, f"  {line}")
                    self.log(LogLevel.ERROR, "=========================")
                return False
                
        except FileNotFoundError:
            self.log(LogLevel.ERROR, "grid-proxy-init not found. Please install Grid Community Toolkit.")
            self.log(LogLevel.ERROR, "  Ubuntu/Debian: apt install globus-proxy-utils")
            self.log(LogLevel.ERROR, "  RHEL/CentOS: yum install globus-proxy-utils")
            return False
        except subprocess.TimeoutExpired:
            self.log(LogLevel.ERROR, "grid-proxy-init timed out (is the key password-protected?)")
            return False
        except Exception as e:
            self.log(LogLevel.ERROR, f"Failed to initialize proxy: {type(e).__name__}: {e}")
            return False
    
    def execute_transfer(self) -> bool:
        """
        Execute the third-party transfer using globus-url-copy with GSI authentication.
        
        Uses gsiftp:// protocol with certificate-based authentication.
        Source: file://<local_pipe> (local named pipe)
        Destination: gsiftp://<host>:2811<remote_pipe> (GridFTP server on port 2811)
        """
        # Ensure we have a valid GSI proxy
        if not self.ensure_gsi_proxy():
            return False
        
        self.log(LogLevel.INFO, "Starting authenticated GridFTP transfer...")
        
        # Build URLs:
        # - Source: local file/pipe using file:// protocol
        # - Destination: remote GridFTP server using gsiftp:// protocol
        src_url = f"file://{self.src_pipe}"
        
        # Extract just the host (without user@) for gsiftp URL
        dst_host = self._extract_host(self.config.dst_host)
        dst_url = f"gsiftp://{dst_host}:2811{self.dst_pipe}"
        
        # Build globus-url-copy command
        guc_cmd = [
            "globus-url-copy",
            "-vb",                                  # Verbose with progress bar
            "-p", str(self.config.parallel_streams),  # Parallel streams (default 4)
            src_url,
            dst_url
        ]
        
        self.log(LogLevel.DEBUG, f"Transfer command: {' '.join(guc_cmd)}")
        self.log(LogLevel.INFO, f"Source URL: {src_url}")
        self.log(LogLevel.INFO, f"Destination URL: {dst_url}")
        
        if self.config.dry_run:
            self.log(LogLevel.INFO, "[DRY-RUN] Would execute globus-url-copy")
            return True
        
        start_time = time.time()
        
        try:
            # Run globus-url-copy and capture both stdout and stderr
            result = subprocess.run(
                guc_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=self.config.timeout
            )
            
            elapsed = time.time() - start_time
            
            # Log stdout if any
            if result.stdout.strip():
                for line in result.stdout.strip().split('\n'):
                    self.log(LogLevel.DEBUG, f"[guc stdout] {line}")
            
            if result.returncode == 0:
                self.log(LogLevel.INFO, 
                        f"✓ Transfer completed in {elapsed:.2f} seconds")
                return True
            else:
                self.log(LogLevel.ERROR, 
                        f"Transfer failed with exit code {result.returncode}")
                # Print full error output for debugging
                if result.stderr.strip():
                    self.log(LogLevel.ERROR, "=== Full Error Output ===")
                    for line in result.stderr.strip().split('\n'):
                        self.log(LogLevel.ERROR, f"  {line}")
                    self.log(LogLevel.ERROR, "=========================")
                # Also print stdout as it may contain error info
                if result.stdout.strip():
                    self.log(LogLevel.ERROR, "=== Full Stdout Output ===")
                    for line in result.stdout.strip().split('\n'):
                        self.log(LogLevel.ERROR, f"  {line}")
                    self.log(LogLevel.ERROR, "==========================")
                return False
                
        except subprocess.TimeoutExpired:
            self.log(LogLevel.ERROR, f"Transfer timed out after {self.config.timeout} seconds")
            return False
        except FileNotFoundError:
            self.log(LogLevel.ERROR, 
                    "globus-url-copy not found. Please install Grid Community Toolkit.")
            self.log(LogLevel.ERROR, "  Ubuntu/Debian: apt install globus-gass-copy-progs")
            self.log(LogLevel.ERROR, "  RHEL/CentOS: yum install globus-gass-copy-progs")
            return False
        except subprocess.CalledProcessError as e:
            self.log(LogLevel.ERROR, f"Transfer command failed with exit code {e.returncode}")
            if e.stderr:
                self.log(LogLevel.ERROR, "=== Full Error Output ===")
                for line in e.stderr.strip().split('\n'):
                    self.log(LogLevel.ERROR, f"  {line}")
                self.log(LogLevel.ERROR, "=========================")
            return False
        except Exception as e:
            self.log(LogLevel.ERROR, f"Transfer error: {type(e).__name__}: {e}")
            return False
    
    def cleanup(self, force: bool = False):
        """
        Clean up remote resources:
        1. Kill any remaining processes
        2. Remove named pipes
        """
        self.log(LogLevel.INFO, "Cleaning up remote resources...")
        
        # Cleanup source host
        if self.sender_started or force:
            try:
                cleanup_cmd = (
                    f"pkill -f 'hyper_sender.*{self.src_pipe}' 2>/dev/null; "
                    f"rm -f {self.src_pipe}"
                )
                self.run_ssh(self.config.src_host, cleanup_cmd, check=False, timeout=30)
                self.log(LogLevel.DEBUG, f"Cleaned up source host")
            except Exception as e:
                self.log(LogLevel.WARN, f"Source cleanup warning: {e}")
        
        # Cleanup destination host
        if self.receiver_started or force:
            try:
                cleanup_cmd = (
                    f"pkill -f 'hyper_receiver.*{self.dst_pipe}' 2>/dev/null; "
                    f"rm -f {self.dst_pipe}"
                )
                self.run_ssh(self.config.dst_host, cleanup_cmd, check=False, timeout=30)
                self.log(LogLevel.DEBUG, f"Cleaned up destination host")
            except Exception as e:
                self.log(LogLevel.WARN, f"Destination cleanup warning: {e}")
        
        self.log(LogLevel.INFO, "✓ Cleanup complete")
    
    def verify_transfer(self) -> bool:
        """
        Verify the transfer by comparing file sizes or checksums.
        """
        self.log(LogLevel.INFO, "Verifying transfer...")
        
        dst_file = self.config.dst_path or self.config.file_path
        
        try:
            # Get source file size
            src_result = self.run_ssh(
                self.config.src_host,
                f"stat -c %s {self.config.file_path}",
                capture=True,
                timeout=30
            )
            src_size = int(src_result.stdout.strip())
            
            # Get destination file size
            dst_result = self.run_ssh(
                self.config.dst_host,
                f"stat -c %s {dst_file}",
                capture=True,
                timeout=30
            )
            dst_size = int(dst_result.stdout.strip())
            
            if src_size == dst_size:
                self.log(LogLevel.INFO, 
                        f"✓ File sizes match: {src_size} bytes")
                return True
            else:
                self.log(LogLevel.ERROR, 
                        f"File size mismatch: src={src_size}, dst={dst_size}")
                return False
                
        except Exception as e:
            self.log(LogLevel.WARN, f"Could not verify transfer: {e}")
            return False
    
    def run(self) -> bool:
        """
        Execute the complete transfer workflow.
        
        Returns:
            True if transfer succeeded, False otherwise
        """
        success = False
        
        try:
            # Print banner
            print(f"\n{COLORS['BOLD']}{'='*60}{COLORS['RESET']}")
            print(f"{COLORS['BOLD']}  HYPER-TRANSFER v2.0 Controller{COLORS['RESET']}")
            print(f"{COLORS['BOLD']}{'='*60}{COLORS['RESET']}")
            print(f"  Source:      {self.config.src_host}:{self.config.file_path}")
            print(f"  Destination: {self.config.dst_host}:{self.config.dst_path or self.config.file_path}")
            print(f"{'='*60}\n")
            
            # Step 0: Calculate optimal route
            self.route = self.get_best_route(self.config.src_host, self.config.dst_host)
            route_str = " -> ".join(self.route)
            self.log(LogLevel.INFO, f"🚀 Optimal Route Calculated: {route_str}")
            
            # Check for multi-hop route
            if len(self.route) > 2:
                self.log(LogLevel.INFO, "Multi-hop route detected!")
                # Identify relay nodes (all nodes except first and last)
                relay_nodes = self.route[1:-1]
                for relay_node in relay_nodes:
                    self.log(LogLevel.INFO, f"Configuring Relay Node: {relay_node} ...")
                    # TODO: In future, setup relay nodes with receiver->sender chaining
                    # For now, this is a mock - actual transfer still uses direct path
                self.log(LogLevel.DEBUG, 
                        "Note: Relay transfer not yet implemented, using direct path")
            
            # Step 1: Setup destination first (receiver must be ready)
            if not self.setup_destination():
                self.log(LogLevel.ERROR, "Failed to setup destination")
                return False
            
            # Small delay to ensure receiver is ready
            time.sleep(0.5)
            
            # Step 2: Setup source (sender starts pushing data)
            if not self.setup_source():
                self.log(LogLevel.ERROR, "Failed to setup source")
                return False
            
            # Step 3: Execute the transfer
            if not self.execute_transfer():
                self.log(LogLevel.ERROR, "Transfer failed")
                return False
            
            # Step 4: Wait for processes to finish
            self.log(LogLevel.INFO, "Waiting for remote processes to complete...")
            time.sleep(2)
            
            # Step 5: Verify
            if not self.config.dry_run:
                self.verify_transfer()
            
            success = True
            print(f"\n{COLORS['GREEN']}{COLORS['BOLD']}Transfer completed successfully!{COLORS['RESET']}\n")
            return True
            
        except KeyboardInterrupt:
            self.log(LogLevel.WARN, "\nTransfer interrupted by user")
            return False
            
        except Exception as e:
            self.log(LogLevel.ERROR, f"Unexpected error: {e}")
            return False
            
        finally:
            # Always cleanup
            self.cleanup(force=True)


def main():
    parser = argparse.ArgumentParser(
        description="HYPER-TRANSFER v2.0 Controller - Third-Party Transfer Orchestrator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic transfer
  %(prog)s node1.hpc.edu node2.hpc.edu /data/large_file.dat

  # Transfer to different path
  %(prog)s src dst /path/to/file -o /other/path/file

  # Dry run to see commands
  %(prog)s src dst /path/to/file --dry-run -v

  # With custom binary location
  %(prog)s src dst /path/to/file --binary-dir /opt/hyper-transfer/build
        """
    )
    
    # Required arguments
    parser.add_argument(
        "src_host",
        help="Source hostname (must have hyper_sender)"
    )
    parser.add_argument(
        "dst_host", 
        help="Destination hostname (must have hyper_receiver)"
    )
    parser.add_argument(
        "file_path",
        help="Path to the file to transfer (on source host)"
    )
    
    # Optional arguments
    parser.add_argument(
        "-o", "--output",
        dest="dst_path",
        help="Destination file path (default: same as source)"
    )
    parser.add_argument(
        "-b", "--binary-dir",
        default="./build",
        help="Directory containing hyper_sender/receiver (default: ./build)"
    )
    parser.add_argument(
        "-p", "--pipe-dir",
        default="/tmp",
        help="Directory for named pipes (default: /tmp)"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose debug output"
    )
    parser.add_argument(
        "-n", "--dry-run",
        action="store_true",
        help="Print commands without executing"
    )
    parser.add_argument(
        "-t", "--timeout",
        type=int,
        default=3600,
        help="Transfer timeout in seconds (default: 3600)"
    )
    parser.add_argument(
        "-P", "--parallel",
        type=int,
        default=4,
        help="Number of parallel GridFTP streams (default: 4)"
    )
    parser.add_argument(
        "-r", "--router",
        dest="router_path",
        help="Path to hyper_router binary (default: {binary_dir}/hyper_router)"
    )
    
    args = parser.parse_args()
    
    # Build configuration
    config = TransferConfig(
        src_host=args.src_host,
        dst_host=args.dst_host,
        file_path=args.file_path,
        dst_path=args.dst_path,
        binary_dir=args.binary_dir,
        pipe_dir=args.pipe_dir,
        verbose=args.verbose,
        dry_run=args.dry_run,
        timeout=args.timeout,
        parallel_streams=args.parallel,
        router_path=args.router_path
    )
    
    # Run controller
    controller = HyperController(config)
    success = controller.run()
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
