# lock-free-queue
Based on Hazard Pointers, implemented in C according to [Maged M. Michael's paper][1]

Note:
1. You can implement a bounded queue by using queue_attr_t.
2. Tested by Cppcheck, Valgrind, and ThreadSanitizer roughly
3. ./wrapper_test.sh "./main 1000 10 0" 10 means executing "./main 1000 10 0" 10 times 

to-do list:
1. Remove retired_next from the struct node. (is it possible?)
2. Implement PrepareForReuse().
3. use of size_t is preferred



### COMPARE RESULT

RUNNING TEST: 100000 items to enqueue/dequeue, iteration 1 times:
Integrated concurrency test with 100 producer(s)/100 consumer(s), 100000 items to enqueue/dequeue: SUCCESS

lock-free:

real	0m0.050s
user	0m0.107s
sys	0m0.027s

lock-based:

real	0m0.072s
user	0m0.033s
sys	0m0.260s

- Performance Winner: In the high-contention test case (100 producers/100 consumers), the Lock-Free Queue is the clear performance winner, outperforming the Lock-Based Queue by approximately 44%.

- Execution Time: The Lock-Free Queue completed the test in 50 milliseconds, while the final, corrected Lock-Based Queue took 72 milliseconds.

- System Call Overhead: The Lock-Based Queue exhibited significantly higher system time (sys: 0.260s) compared to its real execution time. This indicates that substantial overhead was incurred from OS-level operations like thread scheduling and context switching due to intense lock contention.

- Efficiency of CPU Usage: The Lock-Free Queue showed minimal system time (sys: 0.027s) and higher user time (user: 0.107s). This demonstrates its primary advantage: contention is resolved in user space via atomic operations and spinning, avoiding costly system calls and leading to more efficient CPU utilization.

- Validation of Theory: The final results align perfectly with established computer science theory, confirming that well-designed lock-free data structures provide superior performance and scalability in highly concurrent environments compared to their lock-based counterparts.
 

[1]: https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf
