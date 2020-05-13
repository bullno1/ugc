#ifndef UGC_H
#define UGC_H

#include <stddef.h>
#include <stdint.h>

#ifndef UGC_DECL
#define UGC_DECL
#endif

#ifndef UGC_USE_TAGGED_POINTER
#define UGC_USE_TAGGED_POINTER 1
#endif

typedef struct ugc_s ugc_t;
typedef struct ugc_header_s ugc_header_t;

/**
 * @brief Callback function type.
 * @see ugc_init
 */
typedef void(*ugc_visit_fn_t)(ugc_t* gc, ugc_header_t* obj);

enum ugc_state_e
{
	UGC_IDLE,
	UGC_MARK,
	UGC_SWEEP
};

enum ugc_barrier_direction_e
{
	UGC_BARRIER_FORWARD,
	UGC_BARRIER_BACKWARD,
};

/// Header for a managed object. All fields MUST NOT be accessed.
struct ugc_header_s
{
	ugc_header_t* next;
	ugc_header_t* prev;
#if !UGC_USE_TAGGED_POINTER
	unsigned color: 2;
#endif
};

/**
 * @brief Garbage collector data
 *
 * All fields MUST NOT be accessed unless stated otherwise.
 */
struct ugc_s
{
	ugc_header_t set1, set2;
	ugc_header_t *from, *to, *iterator;
	ugc_visit_fn_t scan_fn, release_fn;

	/// Arbitrary userdata, not used by the library.
	void* userdata;

	/// Current state of the garbage collection. Read-only.
	unsigned char state;
	unsigned char white;
};

/**
 * @brief Initialize a GC.
 *
 * Two callbacks are required: one for scanning and the other for releasing.
 *
 * The scan callback must do the following:
 *
 * - If `obj` is NULL, it must call ugc_visit on all root objects.
 * - If `obj` is not NULL, it must call ugc_visit on all objects referenced by
 *   the given object.
 *
 * The release callback will be invoked whenever the GC has determined that
 * an object has become garbage.
 */
UGC_DECL void
ugc_init(ugc_t* gc, ugc_visit_fn_t scan_fn, ugc_visit_fn_t release_fn);

/**
 * @brief Release all allocated objects.
 *
 * This is faster than clearing the root and forcing a full collection.
 */
UGC_DECL void
ugc_release_all(ugc_t* gc);

/// Register a new object to be managed by the GC.
UGC_DECL void
ugc_register(ugc_t* gc, ugc_header_t* obj);

/**
 * @brief Execute a write barrier.
 *
 * Whenever an object stores a reference to another object, this function MUST
 * be called to ensure that the GC works correctly.
 *
 * Root objects (stack, globals) are treated differently so there is no need to
 * call this function when a store to them occurs.
 *
 * @remarks Both objects MUST NOT be NULL.
 */
UGC_DECL void
ugc_write_barrier(
	ugc_t* gc,
	enum ugc_barrier_direction_e direction,
	ugc_header_t* parent,
	ugc_header_t* child
);

/**
 * @brief Make the GC perform one unit of work.
 *
 * What happens depends on the current GC's state.
 *
 * - In UGC_IDLE state, it will scan the root by calling the scan callback then
 *   switch to UGC_MARK state.
 * - In UGC_MARK state, it will mark one object and discover its children using
 *   the scan callback. When there is no object left to mark, the GC will scan
 *   the root once more to account for changes during the mark phase. When all
 *   live objects are marked, it will switch to UGC_SWEEP state.
 * - In UGC_SWEEP state, it will release one object. When all garbage are
 *   released, it wil switch to UGC_IDLE state.
 *
 * @see ugc_init
 */
UGC_DECL void
ugc_step(ugc_t* gc);


/**
 * @brief Perform a collection cycle.
 *
 * Start the GC if it's not already running and only return once the GC has
 * finished collecting all garbage identified at the point of calling.
 *
 * @remarks If the GC is already in the UGC_SWEEP state, it will leave newly
 * created garbage for the next cycle.
 */
UGC_DECL void
ugc_collect(ugc_t* gc);

/**
 * @brief Inform the GC of a referred object during the mark phase.
 *
 * @remarks This function MUST ONLY be called inside the scan callback.
 * @remarks The provided object MUST NOT be NULL.
 * @see ugc_init
 */
UGC_DECL void
ugc_visit(ugc_t* gc, ugc_header_t* obj);

#ifdef UGC_IMPLEMENTATION

#define UGC_GRAY 2

#if UGC_USE_TAGGED_POINTER

#define UGC_PTR(ptr) ((uintptr_t)ptr & (~0x03))
#define UGC_TAG(ptr) ((uintptr_t)ptr & 0x03)
#define UGC_SET_PTR(ptr, val) \
	do { ptr = (void*)((uintptr_t)val | UGC_TAG(ptr)); } while(0)
#define UGC_SET_TAG(ptr, val) \
	do { ptr = (void*)(UGC_PTR(ptr) | (uintptr_t)val); } while(0)

static inline void
ugc_set_next(ugc_header_t* obj, ugc_header_t* value)
{
	UGC_SET_PTR(obj->next, value);
}

static inline ugc_header_t*
ugc_next(ugc_header_t* obj)
{
	return (ugc_header_t*)UGC_PTR(obj->next);
}

static inline void
ugc_set_prev(ugc_header_t* obj, ugc_header_t* value)
{
	UGC_SET_PTR(obj->prev, value);
}

static inline ugc_header_t*
ugc_prev(ugc_header_t* obj)
{
	return (ugc_header_t*)UGC_PTR(obj->prev);
}

static inline void
ugc_set_color(ugc_header_t* obj, unsigned char color)
{
	UGC_SET_TAG(obj->next, color);
}

static inline unsigned char
ugc_color(ugc_header_t* obj)
{
	return (unsigned char)UGC_TAG(obj->next);
}

#else

static inline void
ugc_set_next(ugc_header_t* obj, ugc_header_t* value)
{
	obj->next = value;
}

static inline ugc_header_t*
ugc_next(ugc_header_t* obj)
{
	return obj->next;
}

static inline void
ugc_set_prev(ugc_header_t* obj, ugc_header_t* value)
{
	obj->prev = value;
}

static inline ugc_header_t*
ugc_prev(ugc_header_t* obj)
{
	return obj->prev;
}

static inline void
ugc_set_color(ugc_header_t* obj, unsigned char color)
{
	obj->color = color;
}

static unsigned char
ugc_color(ugc_header_t* obj)
{
	return obj->color;
}

#endif

static void
ugc_push(ugc_header_t* list, ugc_header_t* element)
{
	ugc_set_next(element, list);
	ugc_set_prev(element, list->prev);
	ugc_set_next(list->prev, element);
	ugc_set_prev(list, element);
}

static void
ugc_unlink(ugc_header_t* element)
{
	ugc_header_t* next = ugc_next(element);
	ugc_header_t* prev = ugc_prev(element);

	ugc_set_prev(next, prev);
	ugc_set_next(prev, next);
}

static void
ugc_make_gray(ugc_t* gc, ugc_header_t* obj)
{
	// Move iterator back before moving obj
	if(obj == gc->iterator) { gc->iterator = ugc_prev(obj); }

	ugc_unlink(obj);
	ugc_push(gc->to, obj);
	ugc_set_color(obj, UGC_GRAY);
}

static void
ugc_clear(ugc_header_t* list)
{
	list->next = list;
	list->prev = list;
}

static void
ugc_release_set(ugc_t* gc, ugc_header_t* set)
{
	for(ugc_header_t* itr = ugc_next(set); itr != set;)
	{
		ugc_header_t* next = ugc_next(itr);

		gc->release_fn(gc, itr);

		itr = next;
	}
}

void
ugc_init(ugc_t* gc, ugc_visit_fn_t scan_fn, ugc_visit_fn_t release_fn)
{
	ugc_clear(&gc->set1);
	ugc_clear(&gc->set2);

	gc->state = UGC_IDLE;
	gc->scan_fn = scan_fn;
	gc->release_fn = release_fn;
	gc->white = 0;
	gc->from = &gc->set1;
	gc->to = &gc->set2;
	gc->iterator = gc->to;
	gc->userdata = NULL;
}

void
ugc_register(ugc_t* gc, ugc_header_t* obj)
{
	ugc_push(gc->from, obj);
	ugc_set_color(obj, gc->white);
}

void
ugc_release_all(ugc_t* gc)
{
	ugc_release_set(gc, gc->from);
	ugc_release_set(gc, gc->to);
}

void
ugc_write_barrier(
	ugc_t* gc,
	enum ugc_barrier_direction_e direction,
	ugc_header_t* parent,
	ugc_header_t* child
)
{
	unsigned char white = gc->white;
	unsigned char black = !gc->white;

	if(ugc_color(parent) == black && ugc_color(child) == white)
	{
		switch(direction)
		{
			case UGC_BARRIER_FORWARD:
				ugc_make_gray(gc, child);
				break;
			case UGC_BARRIER_BACKWARD:
				ugc_make_gray(gc, parent);
				break;
		}
	}
}

void
ugc_visit(ugc_t* gc, ugc_header_t* obj)
{
	if(ugc_color(obj) == gc->white)
	{
		ugc_make_gray(gc, obj);
	}
}

void
ugc_step(ugc_t* gc)
{
	ugc_header_t* obj;
	ugc_header_t* to = gc->to;

	switch((enum ugc_state_e)gc->state)
	{
		case UGC_IDLE:
			gc->scan_fn(gc, NULL);
			gc->state = UGC_MARK;
			break;
		case UGC_MARK:
			{
				obj = ugc_next(gc->iterator);
				unsigned char white = gc->white;

				if(obj != to)
				{
					gc->iterator = obj;
					ugc_set_color(obj, !white);
					gc->scan_fn(gc, obj);
				}
				else
				{
					gc->scan_fn(gc, NULL);
					obj = ugc_next(gc->iterator);
					if(obj == to)
					{
						// Since we can get interrupted during the sweep phase,
						// swap "from" and "to" set, flip white color before
						// starting the sweep phase.
						ugc_header_t* from = gc->from;
						gc->from = to;
						gc->to = from;
						gc->white = !white;
						gc->iterator = from->next;
						gc->state = UGC_SWEEP;
					}
				}
			}
			break;
		case UGC_SWEEP:
			obj = gc->iterator;
			if(obj != to)
			{
				gc->iterator = ugc_next(obj);
				gc->release_fn(gc, obj);
			}
			else
			{
				ugc_clear(to);
				gc->state = UGC_IDLE;
			}
			break;
	}
}

void
ugc_collect(ugc_t* gc)
{
	if(gc->state == UGC_IDLE) { ugc_step(gc); }
	while(gc->state != UGC_IDLE) { ugc_step(gc); }
}

#endif

#endif
