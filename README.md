# OS-Jackfruit: Resource-Constrained Container Engine

## 1. Team Information
* **Name:** Shreyas B Patil | **SRN:** PES2UG24CS488
* **Name:** Shreyas Patil   | **SRN:** PES2UG24CS492

---

## 2. Build, Load, and Run Instructions

### Build and Setup
```bash
# Clean previous builds
make clean

# Build the project
make

# Load the kernel monitor module
sudo insmod monitor.ko

# Verify the control device exists
ls -l /dev/container_monitor

# Start the supervisor (Terminal 1)
sudo ./engine supervisor ./rootfs-base

# Create per-container writable rootfs copies (Terminal 2)
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Launch two containers with specific resource limits
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# List currently tracked containers
sudo ./engine ps

# Inspect logs for a specific container
sudo ./engine logs alpha

# Stop active containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Stop the supervisor
# Use Ctrl+C in the supervisor terminal

# Inspect final kernel logs
dmesg | tail

# Unload the kernel module
sudo rmmod monitor
```

## 3. Demo with Screenshots

 Please refer to the official project report for high-resolution screenshots and execution results.

## 4. Engineering Analysis

a. Isolation Mechanisms

    Implementation: The engine achieves isolation by unsharing the PID, UTS, and Mount namespaces before executing chroot into rootfs-base.

    Kernel Role: Namespaces provide a restricted view of system resources; the PID namespace ensures the container sees itself as PID 1, while the UTS namespace allows it to have a unique hostname.

    Filesystem: chroot changes the root directory for the process, preventing it from traversing into the host’s sensitive directories.

    Shared State: The Host Kernel is still shared across all containers. Unlike a VM, containers use the same syscall interface and kernel memory management.

b. Supervisor and Process Lifecycle

    The Parent Role: A long-running supervisor acts as the "Init" for containers, providing a stable execution environment and centralizing telemetry.

    Lifecycle Management: When a container is launched, the supervisor forks a child, applies isolation, and then reaps the process using waitpid to prevent zombie processes.

    Metadata Tracking: The supervisor maintains a thread-safe registry of active PIDs and IDs, allowing the CLI to query the system state via IPC.

    Signal Delivery: Signals like SIGINT (Ctrl+C) are caught by the supervisor and propagated to all children to ensure a clean teardown of Cgroups and namespaces.

c. IPC, Threads, and Synchronization

    Bounded-Buffer Logging: Implements a Producer-Consumer model where containers (producers) write to a shared buffer and the supervisor (consumer) reads from it.

    Race Conditions: Managed to prevent data corruption during simultaneous writes or processing "garbage" data when the buffer is empty.

    Synchronization Logic:

        Mutexes: Used for Mutual Exclusion to ensure only one thread modifies the buffer at a time.

        Condition Variables: Used for Signaling. The producer waits on cond_full if the buffer is capped, while the consumer waits on cond_empty if the buffer is dry.

d. Memory Management and Enforcement

    RSS (Resident Set Size): Measures the portion of a process's memory held in RAM, excluding swapped-out memory or shared libraries.

    Soft vs. Hard Limits:

        Soft Limit: A gentle policy used for resource tracking and warnings via container_monitor without killing the process.

        Hard Limit: A strict policy enforced by Cgroups; if breached, the kernel triggers the OOM (Out Of Memory) killer.

    Kernel-Space Necessity: Enforcement happens in the kernel because user-space processes cannot be trusted to self-regulate; only the kernel has the authority to revoke hardware access instantly.

e. Scheduling Behavior

    Observations: During experiments, the Linux Completely Fair Scheduler (CFS) distributed CPU time evenly among all active containers.

    Fairness: The scheduler ensured all containers and the supervisor remained responsive by assigning specific "time slices."

    Throughput vs. Responsiveness: While high-load containers maximize throughput, the engine prioritizes responsiveness for interactive CLI tasks.
    
    

## 5. Design Decisions and Trade-offs

a. Namespace Isolation

    Design Choice: Utilizing PID, UTS, and Mount namespaces with chroot().

    Trade-off: chroot() is less secure than pivot_root(), as it is theoretically possible to "break out" with root privileges.

    Justification: chroot provides a clear demonstration of isolation without the mounting complexity of pivot_root, making the codebase maintainable for BTech-level implementation.

b. Supervisor Architecture

    Design Choice: A long-running parent supervisor process.

    Trade-off: Creates a Single Point of Failure (SPOF); if the supervisor crashes, management of child containers is lost.

    Justification: Essential for unified logging and metadata tracking; ensures resource cleanup is handled by a single authoritative entity.

c. IPC and Logging

    Design Choice: Bounded-Buffer using Pthread Mutexes and Condition Variables.

    Trade-off: Performance Latency; containers may block if the buffer fills up quickly.

    Justification: Prevents memory exhaustion by ensuring the logging system doesn't consume infinite RAM, prioritizing host stability.

d. Kernel Monitor (Memory Enforcement)

    Design Choice: Custom Kernel Module interacting with Cgroups.

    Trade-off: High risk; a bug in kernel space can cause a Kernel Panic and crash the host OS.

    Justification: Real resource enforcement must be absolute; moving the enforcer to kernel space ensures limits cannot be bypassed.

e. Scheduling Experiments

    Design Choice: Using Static Binaries (stay_alive) for infinite loop testing.

    Trade-off: Unrealistic "stress test" workloads that do not reflect typical application behavior.

    Justification: These workloads force the Linux CFS to intervene, providing visual proof that the supervisor can manage competing processes.
    
    
## 6. Scheduler Experiment Results

PID | USER|  P | NI | VIRT | RE | SHR |S | %CPU | %MEM | TIME+ | COMMAND

5421,  root,  20,  0,  1024,  412,  0,   R, 49.9, 0.1,0 :42.12 ,stay_alive
5423,  root,  20,  0,  1024,  412,  0,   R, 49.7, 0.1,0 :40.05 ,stay_alive


Analysis of Results

    Fairness (CFS Algorithm): CPU usage was split almost exactly 50/50 when the second container was launched, proving the kernel treats isolated containers as equal entities.

    Preemption and Time-Slicing: The supervisor remained responsive even during "greedy" infinite loops, indicating the scheduler successfully preempted container tasks to process logs.   

