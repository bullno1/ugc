#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <munit/munit.h>

#ifndef UGC_IMPLEMENTATION
#define UGC_IMPLEMENTATION
#endif

#include "ugc.h"

typedef struct gc_obj_s gc_obj_t;
typedef struct fixture_s fixture_t;

struct gc_obj_s
{
	ugc_header_t header;
	gc_obj_t* ref;
	bool live;
};

struct fixture_s
{
	ugc_t* gc;
	gc_obj_t* root;
};

static void
scan_gc_obj(ugc_t* gc, ugc_header_t* obj)
{
	if(obj != NULL) // Scan obj
	{
		munit_logf(MUNIT_LOG_INFO, "Scan %p", (void*)obj);
		gc_obj_t* ref = ((gc_obj_t*)obj)->ref;
		if(ref) { ugc_visit(gc, &ref->header); }
	}
	else // Scan root
	{
		munit_log(MUNIT_LOG_INFO, "Scan root");
		fixture_t* fixture = gc->userdata;
		if(fixture->root) { ugc_visit(gc, &fixture->root->header); }
	}
}

static void
free_gc_obj(ugc_t* gc, ugc_header_t* obj_)
{
	(void)gc;
	munit_assert_not_null(obj_);
	munit_logf(MUNIT_LOG_INFO, "Free %p", (void*)obj_);
	gc_obj_t* obj = (gc_obj_t*)obj_;
	munit_assert_true(obj->live);
	obj->live = false;
}

static void*
setup(const MunitParameter params[], void* data)
{
	(void)params;
	(void)data;

	fixture_t* fixture = malloc(sizeof(fixture_t));
	ugc_t* gc = malloc(sizeof(ugc_t));
	ugc_init(gc, scan_gc_obj, free_gc_obj);
	gc->userdata = fixture;
	*fixture = (fixture_t){ .gc = gc };
	return fixture;
}

static void
teardown(void* fixture_)
{
	fixture_t* fixture = fixture_;
	free(fixture->gc);
	free(fixture);
}

static void
alloc(ugc_t* gc, gc_obj_t* obj)
{
	munit_logf(MUNIT_LOG_INFO, "Alloc %p", (void*)obj);
	// Not a real allocation
	obj->live = true;
	obj->ref = NULL;
	ugc_register(gc, &obj->header);
}

static void
set_ref(ugc_t* gc, gc_obj_t* src, gc_obj_t* dst)
{
	munit_logf(MUNIT_LOG_INFO, "Store %p <- %p", (void*)src, (void*)dst);
	src->ref = dst;
	if(dst)
	{
		ugc_write_barrier(
			gc,
			UGC_BARRIER_BACKWARD,
			&src->header,
			&dst->header
		);
	}
}

static MunitResult
basic(const MunitParameter params[], void* fixture_)
{
	(void)params;
	fixture_t* fixture = fixture_;
	ugc_t* gc = fixture->gc;

	gc_obj_t a, b, c;

	alloc(gc, &a);
	alloc(gc, &b);
	alloc(gc, &c);

	munit_assert_true(a.live);
	munit_assert_true(b.live);
	munit_assert_true(c.live);

	set_ref(gc, &a, &b);
	set_ref(gc, &b, &c);

	ugc_collect(gc);

	munit_assert_true(!a.live);
	munit_assert_true(!b.live);
	munit_assert_true(!c.live);

	return MUNIT_OK;
}

static MunitResult
root(const MunitParameter params[], void* fixture_)
{
	(void)params;
	fixture_t* fixture = fixture_;
	ugc_t* gc = fixture->gc;

	gc_obj_t a, b, c;

	alloc(gc, &a);
	alloc(gc, &b);
	alloc(gc, &c);

	set_ref(gc, &a, &c);
	fixture->root = &a;

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(!b.live);
	munit_assert_true(c.live);

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(!b.live);
	munit_assert_true(c.live);

	return MUNIT_OK;
}

static MunitResult
write_barrier(const MunitParameter params[], void* fixture_)
{
	(void)params;
	fixture_t* fixture = fixture_;
	ugc_t* gc = fixture->gc;

	gc_obj_t a, b, c, d;

	alloc(gc, &a);
	alloc(gc, &b);
	alloc(gc, &c);

	set_ref(gc, &a, &b);
	set_ref(gc, &b, &c);
	fixture->root = &a;

	while(ugc_color(&c.header) != !gc->white)
	{
		ugc_step(gc);
	}

	alloc(gc, &d);
	set_ref(gc, &b, &d);

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(b.live);
	munit_assert_true(c.live);
	munit_assert_true(d.live);

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(b.live);
	munit_assert_true(!c.live);
	munit_assert_true(d.live);

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(b.live);
	munit_assert_true(!c.live);
	munit_assert_true(d.live);

	return MUNIT_OK;
}

static MunitResult
root_change(const MunitParameter params[], void* fixture_)
{
	(void)params;
	fixture_t* fixture = fixture_;
	ugc_t* gc = fixture->gc;

	gc_obj_t a, b, c;

	alloc(gc, &a);
	alloc(gc, &b);
	alloc(gc, &c);

	fixture->root = &a;
	set_ref(gc, &a, &b);
	set_ref(gc, &b, &c);

	while(ugc_color(&c.header) != !gc->white)
	{
		ugc_step(gc);
	}

	fixture->root = &b;

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(b.live);
	munit_assert_true(c.live);

	ugc_collect(gc);

	munit_assert_true(!a.live);
	munit_assert_true(b.live);
	munit_assert_true(c.live);

	ugc_collect(gc);

	munit_assert_true(!a.live);
	munit_assert_true(b.live);
	munit_assert_true(c.live);

	return MUNIT_OK;
}

static MunitResult
interupt_sweep(const MunitParameter params[], void* fixture_)
{
	(void)params;
	fixture_t* fixture = fixture_;
	ugc_t* gc = fixture->gc;

	gc_obj_t a, b, c;

	alloc(gc, &a);
	alloc(gc, &b);

	fixture->root = &a;

	while(gc->state != UGC_SWEEP)
	{
		ugc_step(gc);
	}

	alloc(gc, &c);
	set_ref(gc, &a, &c);

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(!b.live);
	munit_assert_true(c.live);

	ugc_collect(gc);

	munit_assert_true(a.live);
	munit_assert_true(!b.live);
	munit_assert_true(c.live);

	return MUNIT_OK;
}

static MunitResult
release_all(const MunitParameter params[], void* fixture_)
{
	(void)params;
	fixture_t* fixture = fixture_;
	ugc_t* gc = fixture->gc;

	gc_obj_t a, b, c;

	alloc(gc, &a);
	alloc(gc, &b);
	alloc(gc, &c);
	a.ref = &b;

	fixture->root = &a;

	for(int i = 0; i < 3; ++i) { ugc_step(gc); }

	ugc_release_all(gc);

	munit_assert_true(!a.live);
	munit_assert_true(!b.live);
	munit_assert_true(!c.live);

	return MUNIT_OK;
}

static MunitTest tests[] = {
	{
		.name = "/basic",
		.test = basic,
		.setup = setup,
		.tear_down = teardown
	},
	{
		.name = "/root",
		.test = root,
		.setup = setup,
		.tear_down = teardown
	},
	{
		.name = "/write_barrier",
		.test = write_barrier,
		.setup = setup,
		.tear_down = teardown
	},
	{
		.name = "/root_change",
		.test = root_change,
		.setup = setup,
		.tear_down = teardown
	},
	{
		.name = "/interupt_sweep",
		.test = interupt_sweep,
		.setup = setup,
		.tear_down = teardown
	},
	{
		.name = "/release_all",
		.test = release_all,
		.setup = setup,
		.tear_down = teardown
	},
	{ .test = NULL }
};

static MunitSuite suite = {
	.prefix = "",
	.tests = tests
};

int
main(int argc, char* argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
