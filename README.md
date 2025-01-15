# lock-free-queue
Based on Hazard Pointers, implemented in C according to Maged M. Michael's paper.

Note:
1. You can implement a bounded queue by using queue_attr_t.
2. Tested by Cppcheck, Valgrind, and ThreadSanitizer roughly

to-do list:
1. Remove retired_next from the struct node. (is it possible?)
2. Implement PrepareForReuse().
