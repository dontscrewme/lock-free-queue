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

[1]: https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf
