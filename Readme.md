Abhinav Krishna Kandukuri PES1UG24CS701
Vatsal Gupta PES1UG24CS702

Engineering Analysis:
Isolation Mechanisms
The runtime achieves isolation using Linux namespaces and filesystem separation. PID namespaces give each container its own process tree, while mount namespaces isolate the filesystem view. Using chroot restricts the container’s root directory so it cannot access host files outside its environment. However, all containers still share the same host kernel, so CPU, memory, and kernel-level resources are globally shared.

Supervisor and Process Lifecycle
A long-running supervisor acts as the parent for all containers, making it easier to manage their lifecycle. It handles process creation, tracks metadata like PID and state, and reaps child processes to prevent zombies. It also allows centralized signal handling, so containers can be stopped or controlled in a consistent way.

IPC, Threads, and Synchronization
The project uses IPC mechanisms like sockets for communication and shared structures for logging. Since multiple threads can access shared data (like container lists or logs), race conditions can happen if not handled properly. Mutexes are used to protect shared data, and coordination mechanisms like condition variables help manage producer-consumer patterns in logging without causing inconsistencies.

Memory Management and Enforcement
RSS (Resident Set Size) measures the actual physical memory used by a process, but it doesn’t account for swapped memory or shared pages perfectly. Soft limits act as warnings when memory usage crosses a threshold, while hard limits enforce strict control by terminating the process. Enforcement is done in kernel space because the kernel has direct access to process memory information and can act reliably in real time.

Scheduling Behavior
The scheduling experiment shows how Linux distributes CPU time based on priority. Processes with lower nice values get more CPU time, while higher nice values get less. This reflects how the scheduler balances fairness, responsiveness, and overall system throughput.



Design Decisions and Tradeoffs

Namespace Isolation
We used Linux namespaces and chroot for isolation. The tradeoff is that it’s lightweight but less secure than full virtualization since everything shares the same kernel. We chose this because it’s efficient and reflects how real container systems work.

Supervisor Architecture
We used a single supervisor to manage all containers. The tradeoff is that it becomes a central point of failure, but it simplifies process management and cleanup. We chose this because it makes lifecycle control much easier.

IPC and Logging
We used sockets and shared logging with synchronization. The tradeoff is added complexity due to handling concurrency and race conditions. We chose this because it enables real-time communication and structured logging.

Kernel Monitor
We implemented memory monitoring in the kernel. The tradeoff is higher complexity and compatibility issues, but it gives accurate and enforceable control. We chose this because user-space monitoring wouldn’t be reliable enough.

Scheduling Experiments
We used nice values to simulate priorities. The tradeoff is that it’s a simplified model, but it clearly shows how CPU scheduling works. We chose this because it demonstrates core concepts without overcomplicating things.


---

Scheduler Experiment Results

PID     NI   %CPU   COMMAND
59870    0   78.7   workload1
59902   15   60.0   workload1

PID     NI   %CPU   COMMAND
60120    0   85.0   workload1
60121   15   45.0   workload1

Processes with lower nice values consistently got more CPU time, but both processes still ran, showing that Linux scheduling balances fairness while still respecting priority.
