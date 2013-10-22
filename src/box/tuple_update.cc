/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "tuple_update.h"

#include "third_party/queue.h"

#include <rope.h>
#include <exception.h>
#include <pickle.h>

/** UPDATE request implementation.
 * UPDATE request is represented by a sequence of operations, each
 * working with a single field. There also are operations which
 * add or remove fields. More than one operation on the same field
 * is allowed.
 *
 * Supported field change operations are: SET, ADD, bitwise AND,
 * XOR and OR, SPLICE.
 *
 * Supported tuple change operations are: SET (when SET field_no
 * == last_field_no + 1), DELETE, INSERT, PUSH and POP.
 * If the number of fields in a tuple is altered by an operation,
 * field index of all following operation is evaluated against the
 * new tuple.
 *
 * Despite the allowed complexity, a typical use case for UPDATE
 * is when the operation count is much less than field count in
 * a tuple.
 *
 * With the common case in mind, UPDATE tries to minimize
 * the amount of unnecessary temporary tuple copies.
 *
 * First, operations are parsed and initialized. Then, the
 * resulting tuple length is calculated. A new tuple is allocated.
 * Finally, operations are applied sequentially, each copying data
 * from the old tuple to the new tuple.
 *
 * With this approach, cost of UPDATE is proportional to O(tuple
 * length) + O(C * log C), where C is the number of operations in
 * the request, and data is copied from the old tuple to the new
 * one only once.
 *
 * There are two special cases in this general scheme, which
 * are handled as follows:
 *
 * 1) As long as INSERT, DELETE, PUSH and POP change the relative
 * field order, an auxiliary data structure is necessary to look
 * up fields in the "old" tuple by field number. Such field
 * index is built on demand, using "rope" data structure.
 *
 * A rope is a binary tree designed to store long strings built
 * from pieces. Each tree node points to a substring of a large
 * string. In our case, each rope node points at a range of
 * fields, initially in the old tuple, and then, as fields are
 * added and deleted by UPDATE, in the "current" tuple.
 * Note, that the tuple itself is not materialized: when
 * operations which affect field count are initialized, the rope
 * is updated to reflect the new field order.
 * In particular, if a field is deleted by an operation,
 * it disappears from the rope and all subsequent operations
 * on this field number instead affect the field following the
 * one.
 *
 * 2) Multiple operations can occur on the same field, and not all
 * operations, by design, can work correctly "in place".
 * For example, SET(4, "aaaaaa") followed by SPLICE(4, 0, 5, 0, * ""),
 * results in zero increase of total tuple length, but requires
 * space to store SET results. To make sure we never go beyond
 * allocated memory, the main loop may allocate a temporary buffer
 * to store intermediate operation results.
 */

STRS(update_op_codes, UPDATE_OP_CODES);

/** Update internal state */
struct tuple_update
{
	region_alloc_func alloc;
	void *alloc_ctx;
	struct rope *rope;
	struct update_op *ops;
	uint32_t op_count;
};

/** Argument of SET operation. */
struct op_set_arg {
	uint32_t length;
	const char *value;
};

/** Argument of ADD, AND, XOR, OR operations. */
struct op_arith_arg {
	uint64_t val;
};

/** Argument of SPLICE. */
struct op_splice_arg {
	int32_t offset;	   /** splice position */
	int32_t cut_length;    /** cut this many bytes. */
	const char *paste; /** paste what? */
	uint32_t paste_length;  /** paste this many bytes. */
};

union update_op_arg {
	struct op_set_arg set;
	struct op_arith_arg arith;
	struct op_splice_arg splice;
};

struct update_field;
struct update_op;

typedef void (*init_op_func)(struct tuple_update *update, struct update_op *op);
typedef void (*do_op_func)(union update_op_arg *arg, const char *in, char *out);

/** A set of functions and properties to initialize and do an op. */
struct update_op_meta {
	init_op_func init_op;
	do_op_func do_op;
	bool works_in_place;
};

/** A single UPDATE operation. */
struct update_op {
	STAILQ_ENTRY(update_op) next;
	struct update_op_meta *meta;
	union update_op_arg arg;
	uint32_t field_no;
	uint32_t max_field_len;
	uint8_t opcode;
};

STAILQ_HEAD(op_list, update_op);

/**
 * We can have more than one operation on the same field.
 * A descriptor of one changed field.
 */
struct update_field {
	/** UPDATE operations against the first field in the range. */
	struct op_list ops;
	/** Points at start of field *data* in the old tuple. */
	const char *old;
	/** End of the old field. */
	const char *tail;
	/**
	 * Length of the "tail" in the old tuple from end
	 * of old data to the beginning of the field in the
	 * next update_field structure.
	 */
	uint32_t tail_len;
};

static void
update_field_init(struct update_field *field,
		  const char *old, uint32_t old_len, uint32_t tail_len)
{
	STAILQ_INIT(&field->ops);
	field->old = old;
	field->tail = old + old_len;
	field->tail_len = tail_len;
}

static inline uint32_t
update_field_len(struct update_field *f)
{
	struct update_op *last = STAILQ_LAST(&f->ops, update_op, next);
	return last ? last->max_field_len : f->tail - f->old;
}

static inline void
op_check_field_no(uint32_t field_no, uint32_t field_max)
{
	if (field_no > field_max)
		tnt_raise(ClientError, ER_NO_SUCH_FIELD, field_no);
}

static inline void
op_adjust_field_no(struct update_op *op, uint32_t field_max)
{
	if (op->field_no == UINT32_MAX)
		op->field_no = field_max;
	else
		op_check_field_no(op->field_no, field_max);
}


static void
do_update_op_set(struct op_set_arg *arg, const char *in __attribute__((unused)),
		 char *out)
{
	memcpy(out, arg->value, arg->length);
}

static void
do_update_op_add(struct op_arith_arg *arg, const char *in, char *out)
{
	if (mp_typeof(*in) != MP_UINT)
		tnt_raise(IllegalParams, "field must be uint");
	uint64_t val = mp_uint_load(&in);
	mp_uint_pack(out, val + arg->val);
}

static void
do_update_op_subtract(struct op_arith_arg *arg, const char *in, char *out)
{
	if (mp_typeof(*in) != MP_UINT)
		tnt_raise(IllegalParams, "field must be uint");
	mp_uint_pack(out, mp_uint_load(&in) - arg->val);
}

static void
do_update_op_and(struct op_arith_arg *arg, const char *in, char *out)
{
	if (mp_typeof(*in) != MP_UINT)
		tnt_raise(IllegalParams, "field must be uint");
	mp_uint_pack(out, mp_uint_load(&in) & arg->val);
}

static void
do_update_op_xor(struct op_arith_arg *arg, const char *in, char *out)
{
	if (mp_typeof(*in) != MP_UINT)
		tnt_raise(IllegalParams, "field must be uint");
	mp_uint_pack(out, mp_uint_load(&in) ^ arg->val);
}

static void
do_update_op_or(struct op_arith_arg *arg, const char *in, char *out)
{
	if (mp_typeof(*in) != MP_UINT)
		tnt_raise(IllegalParams, "field must be uint");
	mp_uint_pack(out, mp_uint_load(&in) | arg->val);
}

static void
do_update_op_splice(struct op_splice_arg *arg, const char *in, char *out)
{
	if (mp_typeof(*in) != MP_STR)
		tnt_raise(IllegalParams, "field must be str");

	uint32_t str_len;
	in = mp_str_load(&in, &str_len);

	if (arg->offset < 0) {
		if (-arg->offset > str_len)
			tnt_raise(ClientError, ER_SPLICE,
				  "offset is out of bound");
		arg->offset = arg->offset + str_len;
	} else if (arg->offset > str_len) {
		arg->offset = str_len;
	}

	assert(arg->offset >= 0 && arg->offset <= str_len);

	if (arg->cut_length < 0) {
		if (-arg->cut_length > (str_len - arg->offset))
			arg->cut_length = 0;
		else
			arg->cut_length += str_len - arg->offset;
	} else if (arg->cut_length > str_len - arg->offset) {
		arg->cut_length = str_len - arg->offset;
	}

	assert(arg->offset <= str_len);

	/* Fill tail part */
	uint32_t tail_offset = arg->offset + arg->cut_length;
	uint32_t tail_length = str_len - tail_offset;

	uint32_t new_str_len = arg->offset + arg->paste_length + tail_length;

	/* Pack string size with random data */
	mp_str_pack(out, out, new_str_len);
	/* Skip MsgPack string header */
	out += mp_str_sizeof(new_str_len) - new_str_len;

	memcpy(out, in, arg->offset);               /* copy field head. */
	out = out + arg->offset;
	memcpy(out, arg->paste, arg->paste_length); /* copy the paste */
	out = out + arg->paste_length;
	memcpy(out, in + tail_offset, tail_length); /* copy tail */
}

static void
do_update_op_insert(struct op_set_arg *arg,
		    const char *in __attribute__((unused)),
		    char *out)
{
	memcpy(out, arg->value, arg->length);
}

static void
init_update_op_insert(struct tuple_update *update, struct update_op *op)
{
	op_adjust_field_no(op, rope_size(update->rope));
	struct update_field *field = (struct update_field *)
		update->alloc(update->alloc_ctx, sizeof(*field));
	update_field_init(field, op->arg.set.value, op->arg.set.length, 0);
	rope_insert(update->rope, op->field_no, field, 1);
}

static void
init_update_op_set(struct tuple_update *update, struct update_op *op)
{
	if (op->field_no < rope_size(update->rope)) {
		struct update_field *field = (struct update_field *)
				rope_extract(update->rope, op->field_no);
		/* Skip all previous ops. */
		STAILQ_INIT(&field->ops);
		STAILQ_INSERT_TAIL(&field->ops, op, next);
		op->max_field_len = op->arg.set.length;
	} else {
		init_update_op_insert(update, op);
	}
}

static void
init_update_op_delete(struct tuple_update *update, struct update_op *op)
{
	op_adjust_field_no(op, rope_size(update->rope) - 1);

	/* Check the operand type, if present. */
	const char *k = op->arg.set.value;
	if (mp_typeof(*k) != MP_UINT)
		tnt_raise(ClientError, ER_ARG_TYPE, op->field_no, "NUM");
	uint64_t delete_count = mp_uint_load(&k);

	if (op->field_no + delete_count > rope_size(update->rope))
		delete_count = rope_size(update->rope) - op->field_no;
	assert(delete_count <= UINT32_MAX);

	if (delete_count == 0)
		tnt_raise(ClientError, ER_UPDATE_FIELD,
			  op->field_no, "cannot delete 0 fields");

	for (uint32_t u = 0; u < (uint32_t) delete_count; u++)
		rope_erase(update->rope, op->field_no);
}

static void
init_update_op_arith(struct tuple_update *update, struct update_op *op)
{
	op_check_field_no(op->field_no, rope_size(update->rope) - 1);

	struct update_field *field = (struct update_field *)
			rope_extract(update->rope, op->field_no);
	struct op_arith_arg *arg = &op->arg.arith;

	const char *value = op->arg.set.value;
	/* TODO: signed int & float support */
	if (mp_typeof(*value) != MP_UINT)
		tnt_raise(ClientError, ER_ARG_TYPE, op->field_no, "UINT");
	arg->val = mp_uint_load(&value);
	STAILQ_INSERT_TAIL(&field->ops, op, next);
	op->max_field_len = mp_uint_sizeof(UINT64_MAX);
}

static void
init_update_op_splice(struct tuple_update *update, struct update_op *op)
{
	op_check_field_no(op->field_no, rope_size(update->rope) - 1);
	struct update_field *field = (struct update_field *)
			rope_extract(update->rope, op->field_no);

	struct op_splice_arg *arg = &op->arg.splice;
	const char *value = op->arg.set.value;
	const char *end = value + op->arg.set.length;

	if (mp_typeof(*value) == MP_STR) {
		/*
		 * Backward compatible solution for
		 * box.update(space, key, ':p', field_no,
		 *    box.pack('ppp', offset, cut_length, past})
		 */
		uint32_t len;
		value = mp_str_load(&value, &len);
	} else {
		/*
		 * box.update(space, key, ':p', field_no,
		 *    {offset, cut_length, paste})
		 */
		uint32_t size;
		if (mp_typeof(*value) != MP_ARRAY ||
		    (size = mp_array_load(&value)) != 3) {
			tnt_raise(IllegalParams, "Please use "
				  "{offset, cut_length, paste} as "
				  "third argument");
		}
	}

	/* TODO: signed int & float support */
	if (unlikely(mp_typeof(*value) != MP_UINT))
		tnt_raise(ClientError, ER_ARG_TYPE, op->field_no, "UINT");
	arg->offset = mp_uint_load(&value);

	if (unlikely(mp_typeof(*value) != MP_UINT))
		tnt_raise(ClientError, ER_ARG_TYPE, op->field_no, "UINT");
	arg->cut_length = mp_uint_load(&value); /* cut length */

	if (unlikely(mp_typeof(*value) != MP_UINT))
		tnt_raise(ClientError, ER_ARG_TYPE, op->field_no, "UINT");
	arg->paste = mp_str_load(&value, &arg->paste_length); /* value */

	/* Check that the operands are fully read. */
	if (value != end)
		tnt_raise(IllegalParams, "field splice format error");

	/* Record the new field length (maximal). */
	op->max_field_len = mp_str_sizeof(update_field_len(field) +
					  arg->paste_length);
	STAILQ_INSERT_TAIL(&field->ops, op, next);
}

static struct update_op_meta update_op_meta[UPDATE_OP_MAX + 1] = {
	{ init_update_op_set, (do_op_func) do_update_op_set, true },
	{ init_update_op_arith, (do_op_func) do_update_op_add, true },
	{ init_update_op_arith, (do_op_func) do_update_op_and, true },
	{ init_update_op_arith, (do_op_func) do_update_op_xor, true },
	{ init_update_op_arith, (do_op_func) do_update_op_or, true },
	{ init_update_op_splice, (do_op_func) do_update_op_splice, false },
	{ init_update_op_delete, (do_op_func) NULL, true },
	{ init_update_op_insert, (do_op_func) do_update_op_insert, true },
	{ init_update_op_arith, (do_op_func) do_update_op_subtract, true },
};

/** Split a range of fields in two, allocating update_field
 * context for the new range.
 */
static void *
update_field_split(void *split_ctx, void *data, size_t size __attribute__((unused)),
		   size_t offset)
{
	struct tuple_update *update = (struct tuple_update *) split_ctx;

	struct update_field *prev = (struct update_field *) data;

	struct update_field *next = (struct update_field *)
			update->alloc(update->alloc_ctx, sizeof(*next));
	assert(offset > 0 && prev->tail_len > 0);

	const char *field = prev->tail;
	const char *end = field + prev->tail_len;

	for (uint32_t i = 1; i < offset; i++) {
		mp_load(&field);
	}

	prev->tail_len = field - prev->tail;
	const char *f = field;
	mp_load(&f);
	uint32_t field_len = f - field;

	update_field_init(next, field, field_len, end - field - field_len);
	return next;
}

/** Free rope node - do nothing, since we use a pool allocator. */
static void
region_alloc_free_stub(void *ctx, void *mem)
{
	(void) ctx;
	(void) mem;
}

/**
 * We found a tuple to do the update on. Prepare and optimize
 * the operations.
 */
void
update_create_rope(struct tuple_update *update,
		   const char *tuple_data, const char *tuple_data_end)
{
	uint32_t field_count = mp_array_load(&tuple_data);

	update->rope = rope_new(update_field_split, update, update->alloc,
				region_alloc_free_stub, update->alloc_ctx);

	/* Initialize the rope with the old tuple. */

	struct update_field *first = (struct update_field *)
			update->alloc(update->alloc_ctx, sizeof(*first));
	const char *field = tuple_data;
	const char *end = tuple_data_end;

	/* Add first field to rope */
	mp_load(&tuple_data);
	uint32_t field_len = tuple_data - field;
	update_field_init(first, field, field_len,
			  end - field - field_len);

	rope_append(update->rope, first, field_count);
	for (uint32_t i = 0; i < update->op_count; i++) {
		update->ops[i].meta->init_op(update, &update->ops[i]);
	}
}

static uint32_t
update_calc_max_tuple_length(struct tuple_update *update)
{
	struct rope_iter it;
	struct rope_node *node;

	rope_iter_create(&it, update->rope);
	uint32_t max_tuple_size = 0;
	for (node = rope_iter_start(&it); node; node = rope_iter_next(&it)) {
		struct update_field *field =
				(struct update_field *) rope_leaf_data(node);
		uint32_t field_len = update_field_len(field);
		max_tuple_size += (field_len + field->tail_len);
	}

	max_tuple_size += mp_array_sizeof(rope_size(update->rope));
	return max_tuple_size;
}

static uint32_t
do_update_ops(struct tuple_update *update, char *buffer, char *buffer_end)
{
	char *new_data = buffer;
	char *new_data_end = buffer_end;
	new_data = mp_array_pack(new_data, rope_size(update->rope));

	uint32_t total_field_count = 0;

	struct rope_iter it;
	struct rope_node *node;

	rope_iter_create(&it, update->rope);
	for (node = rope_iter_start(&it); node; node = rope_iter_next(&it)) {
		struct update_field *field = (struct update_field *)
				rope_leaf_data(node);
		uint32_t field_count = rope_leaf_size(node);
		uint32_t field_len = update_field_len(field);

		const char *old_field = field->old;
		char *new_field = (STAILQ_EMPTY(&field->ops) ?
					   (char *) old_field : new_data);
		struct update_op *op;
		STAILQ_FOREACH(op, &field->ops, next) {
			/*
			 * Pre-allocate a temporary buffer when the
			 * subject operation requires it, i.e.:
			 * - op overwrites data while reading it thus
			 *   can't work with in == out (SPLICE)
			 * - op result doesn't fit into the new tuple
			 *   (can happen when a big SET is then
			 *   shrunk by a SPLICE).
			 */
			if ((old_field == new_field &&
			     !op->meta->works_in_place) ||
			    /*
			     * Sic: this predicate must function even if
			     * new_field != new_data.
			     */
			    new_data + op->max_field_len > new_data_end) {
				/*
				 * Since we don't know which of the two
				 * conditions above got us here, simply
				 * palloc a *new* buffer of sufficient
				 * size.
				 */
				new_field = (char *) update->alloc(
					update->alloc_ctx, op->max_field_len);
			}
			assert(op->meta != NULL);
			op->meta->do_op(&op->arg, old_field, new_field);
			/* Next op uses previous op output as its input. */
			old_field = new_field;
		}
		/*
		 * Make sure op results end up in the tuple, copy
		 * tail_len from the old tuple.
		*/
		const char *f = new_field;
		mp_load(&f);
		field_len = f - new_field;
		if (new_field != new_data)
			memcpy(new_data, new_field, field_len);

		f = new_data;

		new_data += field_len;
		assert(field->tail_len == 0 || field_count > 1);
		if (field_count > 1) {
			memcpy(new_data, field->tail, field->tail_len);
			new_data += field->tail_len;
		}
		total_field_count += field_count;
	}

	assert(rope_size(update->rope) == total_field_count);
	assert(new_data <= buffer_end);
	return new_data - buffer; /* real_tuple_size */
}

static void
update_read_ops(struct tuple_update *update, const char *expr,
		const char *expr_end)
{
	/* number of operations */
	update->op_count = pick_u32(&expr, expr_end);

	if (update->op_count > BOX_UPDATE_OP_CNT_MAX)
		tnt_raise(IllegalParams, "too many operations for update");
	if (update->op_count == 0)
		tnt_raise(IllegalParams, "no operations for update");

	/* Read update operations.  */
	update->ops = (struct update_op *) update->alloc(update->alloc_ctx,
				update->op_count * sizeof(struct update_op));
	struct update_op *op = update->ops;
	struct update_op *ops_end = op + update->op_count;
	for (; op < ops_end; op++) {
		/* Read operation */
		op->field_no = pick_u32(&expr, expr_end);
		op->opcode = pick_u8(&expr, expr_end);

		if (op->opcode >= UPDATE_OP_MAX)
			tnt_raise(ClientError, ER_UNKNOWN_UPDATE_OP);
		op->meta = &update_op_meta[op->opcode];

		/* Read MP value with type checking */
		op->arg.set.value = expr;
		/* test that arg is valid MsgPack */
		if (unlikely(!mp_check(&expr, expr_end)))
			tnt_raise(IllegalParams, "Invalid MsgPack");
		op->arg.set.length = expr - op->arg.set.value;
	}

	/* Check the remainder length, the request must be fully read. */
	if (expr != expr_end)
		tnt_raise(IllegalParams, "can't unpack update operations");
}

const char *
tuple_update_execute(region_alloc_func alloc, void *alloc_ctx,
		     const char *expr,const char *expr_end,
		     const char *old_data, const char *old_data_end,
		     uint32_t *p_new_size)
{
	struct tuple_update *update = (struct tuple_update *)
			alloc(alloc_ctx, sizeof(*update));
	assert(update != NULL);
	memset(update, 0, sizeof(*update));
	update->alloc = alloc;
	update->alloc_ctx = alloc_ctx;

	update_read_ops(update, expr, expr_end);
	update_create_rope(update, old_data, old_data_end);
	uint32_t max_tuple_size = update_calc_max_tuple_length(update);

	char *buffer = (char *) alloc(alloc_ctx, max_tuple_size);
	char *buffer_end = buffer + max_tuple_size;

	uint32_t real_tuple_size = do_update_ops(update, buffer, buffer_end);

	*p_new_size = real_tuple_size;

	return buffer;
}
