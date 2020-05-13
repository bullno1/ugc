#include <theft/theft.h>
#include <theft/theft_mt.h>
#include <stdlib.h>
#include <string.h>
#ifndef UGC_IMPLEMENTATION
#define UGC_IMPLEMENTATION
#endif
#include "ugc.h"

typedef struct gc_obj_s gc_obj_t;
typedef struct gc_ref_info_s gc_ref_info_t;

enum gc_op_type_e
{
	GC_ALLOC,
	GC_SET_REF_BACKWARD,
	GC_SET_REF_FORWARD,
	GC_CLEAR_REF,
	GC_STEP,
	GC_COLLECT,

	GC_COUNT
};

struct gc_obj_s
{
	ugc_header_t header;

	unsigned int num_frees;
	bool visited;
	size_t num_refs;
	gc_obj_t** refs;
};

struct gc_roots_s
{
	size_t len;
	gc_obj_t** slots;
};

struct gc_ref_info_s
{
	uint32_t root_index;
	uint32_t obj_ref_index;
	gc_obj_t* obj;
	gc_obj_t** ref;
};

struct trial_env_s
{
	void* params[5];
};

static void*
alloc_small_number(struct theft* t, theft_seed seed, void* env)
{
	(void)seed;
	(void)env;
	return (void*)(theft_random(t) % 10 + 1);
}

static void*
alloc_medium_number(struct theft* t, theft_seed seed, void* env)
{
	(void)seed;
	(void)env;
	return (void*)(theft_random(t) % 50 + 1);
}

static void*
alloc_large_number(struct theft* t, theft_seed seed, void* env)
{
	(void)seed;
	(void)env;
	return (void*)(theft_random(t) % 100 + 1);
}

static void*
alloc_seed(struct theft* t, theft_seed seed, void* env)
{
	(void)t;
	(void)env;
	return (void*)seed;
}

static void*
shrink_number(void* instance, uint32_t tactic, void* env)
{
	(void)env;
	uint32_t number = (uint32_t)(uintptr_t)instance;
	if(number == 0) { return THEFT_DEAD_END; }

	switch(tactic)
	{
		case 0: return (void*)(uintptr_t)(number / 2);
		case 1: return number > 0 ? (void*)(uintptr_t)(number - 1) : THEFT_DEAD_END;
		default: return THEFT_NO_MORE_TACTICS;
	}
}

static void*
grow_number(void* instance, uint32_t tactic, void* env)
{
	(void)env;
	uint32_t number = (uint32_t)(uintptr_t)instance;

	switch(tactic)
	{
		case 0: return (void*)(uintptr_t)(number * 2);
		case 1: return (void*)(uintptr_t)(number + 1);
		default: return THEFT_NO_MORE_TACTICS;
	}
}

static theft_hash
hash_number(void* instance, void* env)
{
	(void)env;
	return theft_hash_onepass((void*)&instance, sizeof(void*));
}

void print_number(FILE* f, void* instance, void* env)
{
	(void)env;
	fprintf(f, "%zu", (uintptr_t)instance);
}

static void
scan_obj(ugc_t* gc, ugc_header_t* header)
{
	size_t num_slots;
	gc_obj_t** slots;

	if(header != NULL)
	{
		gc_obj_t* obj = (gc_obj_t*)header;
		num_slots = obj->num_refs;
		slots = obj->refs;
	}
	else
	{
		struct gc_roots_s* roots = gc->userdata;
		num_slots = roots->len;
		slots = roots->slots;
	}

	for(size_t i = 0; i < num_slots; ++i)
	{
		gc_obj_t* obj = slots[i];
		if(obj != NULL) { ugc_visit(gc, &obj->header); }
	}
}

static void
release_obj(ugc_t* gc, ugc_header_t* header)
{
	(void)gc;
	gc_obj_t* obj = (gc_obj_t*)header;
	++obj->num_frees;

}

static void
mark_slots(size_t len, gc_obj_t** slots)
{
	for(size_t i = 0; i < len; ++i)
	{
		gc_obj_t* obj = slots[i];
		if(obj != NULL && !obj->visited)
		{
			obj->visited = true;
			mark_slots(obj->num_refs, obj->refs);
		}
	}
}

static gc_ref_info_t
pick_ref(struct theft_mt* mt, size_t num_roots, gc_obj_t** root_slots)
{
	uint32_t root_index = theft_mt_random(mt) % num_roots;
	gc_obj_t** ref = &root_slots[root_index];
	bool target_root = false
		|| *ref == NULL
		|| (*ref)->num_refs == 0
		|| theft_mt_random(mt) % 2;
	if(target_root)
	{
		return (gc_ref_info_t){
			.root_index = root_index,
			.ref = ref
		};
	}
	else
	{
		gc_obj_t* obj = *ref;
		uint32_t obj_ref_index = theft_mt_random(mt) % obj->num_refs;
		return (gc_ref_info_t){
			.root_index = root_index,
			.obj = *ref,
			.obj_ref_index = obj_ref_index,
			.ref = &(obj->refs[obj_ref_index])
		};
	}
}

static theft_trial_res
simulate_gc(
	bool verbose,
	void* seed_,
	void* num_roots_,
	void* max_objs_,
	void* num_ops_,
	void* num_drops_
)
{
	theft_seed seed = (uint32_t)(uintptr_t)seed_;
	uint32_t num_roots = (uint32_t)(uintptr_t)num_roots_;
	uint32_t max_objs = (uint32_t)(uintptr_t)max_objs_;
	uint32_t num_ops = (uint32_t)(uintptr_t)num_ops_;
	uint32_t num_drops = (uint32_t)(uintptr_t)num_drops_;

#define LOG(...) do { if(verbose) { printf(__VA_ARGS__); } } while(0)
	if(num_roots == 0 || num_drops > num_ops) { return THEFT_TRIAL_SKIP; }

	struct theft_mt* mt = theft_mt_init(seed);
	gc_obj_t** root_slots = calloc(num_roots, sizeof(gc_obj_t*));
	gc_obj_t* objs = calloc(max_objs, sizeof(gc_obj_t));
	struct gc_roots_s roots = {
		.len = num_roots,
		.slots = root_slots
	};
	ugc_t gc;
	ugc_init(&gc, scan_obj, release_obj);
	gc.userdata = &roots;
	size_t num_objs = 0;

	LOG("-----------------------\n");
	LOG("Seed: %04zu\n", seed);
	LOG("Max objs: %d\n", max_objs);
	LOG("Num roots: %d\n", num_roots);
	LOG("Num ops: %d\n", num_ops);
	LOG("Num drops: %d\n", num_drops);
	LOG("-----------------------\n");

	for(size_t i = 0; i < num_ops; ++i)
	{
start:
		switch(theft_mt_random(mt) % GC_COUNT)
		{
			case GC_ALLOC:
				{
					if(num_objs == max_objs) { goto start; }

					size_t root_slot = theft_mt_random(mt) % num_roots;
					size_t num_refs = theft_mt_random(mt) % 10;

					if(num_drops > 0) { --num_drops; continue; }

					gc_obj_t* obj = &objs[num_objs++];
					obj->num_refs = num_refs;
					obj->refs = calloc(num_refs, sizeof(gc_obj_t*));
					ugc_register(&gc, &obj->header);
					root_slots[root_slot] = obj;

					LOG("root[%zu] <- new Obj(%zu) // #%zu\n", root_slot, num_refs, num_objs - 1);
				}
				break;
			case GC_SET_REF_BACKWARD:
			case GC_SET_REF_FORWARD:
				{
					gc_ref_info_t src_ref_info = pick_ref(mt, num_roots, root_slots);
					gc_ref_info_t dst_ref_info = pick_ref(mt, num_roots, root_slots);
					enum gc_op_type_e op = theft_mt_random(mt) % GC_COUNT;

					if(num_drops > 0) { --num_drops; continue; }

					LOG("root[%d]", src_ref_info.root_index);
					if(src_ref_info.obj)
					{
						LOG("[%d]", src_ref_info.obj_ref_index);
					}
					LOG(" <- ");
					LOG("root[%d]", dst_ref_info.root_index);
					if(dst_ref_info.obj)
					{
						LOG("[%d]", dst_ref_info.obj_ref_index);
					}
					LOG(" // ");
					if(op == GC_SET_REF_FORWARD)
					{
						LOG("UGC_BARRIER_FORWARD");
					}
					else
					{
						LOG("UGC_BARRIER_BACKWARD");
					}

					LOG("\n");

					*src_ref_info.ref = *dst_ref_info.ref;

					if(src_ref_info.obj && *dst_ref_info.ref != NULL)
					{
						ugc_write_barrier(
							&gc,
							op == GC_SET_REF_FORWARD ? UGC_BARRIER_FORWARD : UGC_BARRIER_BACKWARD,
							&src_ref_info.obj->header,
							&(*dst_ref_info.ref)->header
						);
					}
				}
				break;
			case GC_CLEAR_REF:
				{
					gc_ref_info_t ref_info = pick_ref(mt, num_roots, root_slots);

					if(num_drops > 0) { --num_drops; continue; }

					*ref_info.ref = NULL;

					LOG("root[%d]", ref_info.root_index);
					if(ref_info.obj)
					{
						LOG("[%d]", ref_info.obj_ref_index);
					}
					LOG(" <- null\n");
				}
				break;
			case GC_STEP:
				{
					if(num_drops > 0) { --num_drops; continue; }

					const char* gc_state_names[] = { "IDLE", "MARK", "SWEEP" };
					enum ugc_state_e old_state = gc.state;
					ugc_step(&gc);
					enum ugc_state_e new_state = gc.state;
					LOG("gc_step(): %s -> %s\n", gc_state_names[old_state], gc_state_names[new_state]);
				}
				break;
			case GC_COLLECT:
				if(theft_mt_random(mt) % 2 != 0) { goto start; }

				if(num_drops > 0) { --num_drops; continue; }

				ugc_collect(&gc);
				LOG("gc_collect()\n");
				break;
		}
	}

	ugc_collect(&gc);
	ugc_collect(&gc);

	mark_slots(num_roots, root_slots);
	bool correct = true;
	for(size_t i = 0; i < num_objs; ++i)
	{
		gc_obj_t* obj = &objs[i];

		unsigned int expected_num_frees = obj->visited ? 0 : 1;
		if(expected_num_frees != obj->num_frees)
		{
			LOG("-----------\n");
			LOG("obj#%zu.num_frees is %d instead of %d\n", i, obj->num_frees, expected_num_frees);
			correct = false;
			break;
		}
	}

	for(size_t i = 0; i < num_objs; ++i)
	{
		gc_obj_t* obj = &objs[i];
		free(obj->refs);
	}

	free(objs);
	free(root_slots);
	theft_mt_free(mt);

	return correct ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static theft_trial_res
check_gc_correctness(
	void* seed, void* num_roots, void* max_objs,
	void* num_ops, void* num_drops
)
{
	return simulate_gc(false, seed, num_roots, max_objs, num_ops, num_drops);
}

static theft_progress_callback_res
report_progress(struct theft_trial_info* info, void* env)
{
	struct trial_env_s* trial_env = env;
	void** params = trial_env->params;
	switch(info->status)
	{
		case THEFT_TRIAL_PASS:
			fprintf(stdout, ".");
			fflush(stdout);
			break;
		case THEFT_TRIAL_FAIL:
			{
				uint32_t num_ops = (uint32_t)(uintptr_t)info->args[3];
				uint32_t num_drops = (uint32_t)(uintptr_t)info->args[4];
				uint32_t num_effective_ops = num_ops - num_drops;
				uint32_t current_num_effective_ops =
					(uint32_t)(uintptr_t)params[3] - (uint32_t)(uintptr_t)params[4];
				if(params[3] == 0 || current_num_effective_ops > num_effective_ops)
				{
					memcpy(trial_env->params, info->args, sizeof(void*) * 5);
				}
			}
			break;
		default:
			break;
	}
	return THEFT_PROGRESS_CONTINUE;
}

int main()
{
	struct theft_type_info shrinking_small_number = {
		.alloc = alloc_small_number,
		.hash = hash_number,
		.shrink = shrink_number,
		.print = print_number
	};

	struct theft_type_info shrinking_medium_number = {
		.alloc = alloc_medium_number,
		.hash = hash_number,
		.shrink = shrink_number,
		.print = print_number
	};

	struct theft_type_info shrinking_large_number = {
		.alloc = alloc_large_number,
		.hash = hash_number,
		.shrink = shrink_number,
		.print = print_number
	};

	struct theft_type_info growing_number = {
		.alloc = alloc_small_number,
		.hash = hash_number,
		.shrink = grow_number,
		.print = print_number
	};

	struct theft_type_info seed = {
		.alloc = alloc_seed,
		.hash = hash_number,
		.print = print_number
	};

	struct trial_env_s trial_env = { .params = { 0 } };

	struct theft_cfg cfg = {
		.name = "check_gc_correctness",
		.fun = check_gc_correctness,
		.type_info = {
			&seed,
			&shrinking_small_number,
			&shrinking_medium_number,
			&shrinking_large_number,
			&growing_number
		},
		.trials = 100000,
		.progress_cb = report_progress,
		.env = &trial_env
	};

	struct theft* t = theft_init(0);
	theft_run_res result = theft_run(t, &cfg);
	theft_free(t);

	if((uint32_t)(uintptr_t)trial_env.params[3] > 0)
	{
		printf("\n");
		simulate_gc(true,
			trial_env.params[0],
			trial_env.params[1],
			trial_env.params[2],
			trial_env.params[3],
			trial_env.params[4]
		);
	}

	return result == THEFT_RUN_PASS ? 0 : 1;
}
