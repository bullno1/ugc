# μgc

[![License](https://img.shields.io/badge/license-BSD-blue.svg)](LICENSE)

*μgc* is a single-header garbage collector library.
It is designed to be embedded in a programming language's runtime.

## Features

* Fully incremental using tri-color marking.
* No external dependencies.
* No memory allocation.
* Overhead of 2 pointers per object.
* Thoroughly tested: [unit test](munit.c) and [property-based test](theft.c)

## Usage

### Building

Put `ugc.h` into your project.

Write in *one* C file:

```c
#define UGC_IMPLEMENTATION
#include "ugc.h"
```

### Integrating with your runtime

All heap allocated objects must include `ugc_header_t`, usually as the first member:

```c
struct my_heap_obj_s
{
	ugc_header_t header;

	// Other fields
	type_info_t type;
	void* external_ref;
	// ...
};
```

Initialize a garbage collector with:

```
ugc_t gc;

ugc_init(&gc, scan_fn, free_fn);

// Each gc instance has a userdata field.
gc.userdata = language_runtime;
```

`ugc_init` requires two callback functions: `scan_fn` and `free_fn`.

`scan_fn` is a function that will be called to help μgc trace an object's reference and the root set.
It should behave as follow:

```c
static void
scan_gc_obj(ugc_t* gc, ugc_header_t* header)
{
	if(header == NULL) // Scan the root set
	{
		// ugc_visit needs to be called on each pointer in the stack and global
		// environment

		struct my_language_runtime_s* runtime = gc->userdata;

		// Scan the stack
		for(unsigned int i = 0; i < runtime->stack_len; ++i)
		{
			if(runtime->stack[i]) { ugc_visit(gc, runtime->stack[i]); }
		}

		// If the language is similar to Lua where the global environment is a
		// first-class object, call ugc_visit on the object instead of its fields
		ugc_visit(gc, runtime->global);
	}
	else // Scan the given object
	{
		// ugc_visit needs to be called on each external reference of this
		// object.

		struct my_heap_obj_s* obj = (struct my_heap_obj_s*)header;
		if(obj->external_ref) { ugc_visit(gc, obj->external_ref); }
	}
}
```

The second parameter of `ugc_visit` must not be NULL and must point to a `ugc_header_t`.

`free_fn` will be called when μgc has determined that a language's object is garbage.
It should release an object's resources:

```c
static void
free_gc_obj(ugc_t* gc, ugc_header_t* header)
{
	struct my_language_runtime_s* runtime = gc->userdata;
	runtime->free(header);
}
```

When a new object is allocated, it needs to be registered with μgc using:

```
ugc_register(gc, new_object);
```

Whenever an object receives a reference to another (i.e: `src.field = dst`), μgc must be informed:

```
ugc_write_barrier(gc, direction, src, dst);
```

There are two types of write barriers: "forward" and "backward".

[LuaJIT wiki](http://wiki.luajit.org/New-Garbage-Collector#gc-algorithms_tri-color-incremental-mark-sweep) states the following about "backward" barrier:

> This is moving the barrier "back", because the object has to be reprocessed later on.
> This is beneficial for container objects, because they usually receive several stores in succession.
> This avoids a barrier for the next objects that are stored into it (which are likely white, too).

And "forward" barrier:

> This moves the barrier "forward", because it implicitly drives the GC forward.
> This works best for objects that only receive isolated stores.

In the above language example, stores to the stack/local variables do not require a write barrier but stores to global variables do.

### Controlling garbage collection

μgc does not start collection automatically because there are many factors (e.g: heap size, number of objects, time limit...) that need to be considered.
It is best left to the language implementer to decide.
Thus, it provides two functions to control the garbage collector:

`ugc_step(gc)` performs a single atomic step (e.g: mark/free one object, scan the root set).
One typically calls it several times per allocation or regularly after a trigger.
"Push/pop GC pause" function usually found in some language's API can be implemented by simply maintaining a counter and not calling `ugc_step` if it is non-zero.
The current state of the GC is stored in `ugc_t::state`.
One can use that to give different speeds to different phases.

`ugc_collect(gc)` finishes the *current* collection cycle.
This means:

- If the GC has alread started, it will return once the current cycle ends.
- If the GC is idle (`ugc_t::state == UGC_IDLE`), it will start a cycle and finish it.

Therefore, if the GC is already in the `UGC_SWEEP` phase, any new garbage will be left to the next cycle.
One needs to pay attention to this when calling `gc_collect` in case of emergency (e.g: `malloc` returns `NULL`).
The first call to `gc_collect` may reclaim some but not all memory.
It is possible for `malloc` to return `NULL` again.
`gc_collect` should be called a second time.
The runtime should only panic if `malloc` still fails.
