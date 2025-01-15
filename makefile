# Define compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O3 -fsanitize=thread #-fno-omit-frame-pointer -fsanitize=address #-fsanitize=thread #
INCLUDES = -I/home/firststop0907/linkedList_queue
LDFLAGS = -lpthread

# List of source files
SRCS = $(wildcard *.c)
# List of object files (derived from source files)
OBJS = $(SRCS:.c=.o)
# Name of the final executable
EXEC = main

# Default target to build the executable
all: $(EXEC)

# Link object files to create the executable
$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $(EXEC)

# Rule to compile .c files to .o files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(EXEC)

# PHONY targets to ensure `make` works correctly with these names
.PHONY: all clean