#include <r_bin.h>
#include <r_core.h>
#include <r_anal.h>
#include "../bin/pdb/types.h"

static bool is_parsable_type (const ELeafType type) {
	return (type == eLF_STRUCTURE ||
		type == eLF_UNION ||
		type == eLF_ENUM ||
		type == eLF_CLASS);
}

/**
 * @brief Create a type name from offset
 * 
 * @param offset 
 * @return char* Name or NULL if error
 */
static char *create_type_name_from_offset(ut64 offset) {
	int offset_length = snprintf (NULL, 0, "type_0x%" PFMT64x, offset);
	char *str = malloc (offset_length + 1);
	snprintf (str, offset_length + 1, "type_0x%" PFMT64x, offset);
	return str;
}

static RAnalStructMember *parse_member(STypeInfo *type_info, RList *types) {
	r_return_val_if_fail (type_info && types, NULL);
	// ignore LF_METHOD, LF_NESTTYPE, bitfields, etc for now TODO
	if (type_info->leaf_type != eLF_MEMBER) {
		return NULL;
	}
	r_return_val_if_fail (type_info->get_name &&
			type_info->get_print_type && type_info->get_val, NULL);
	char *name = NULL;
	char *type = NULL;
	int offset = 0;

	type_info->get_val (type_info, &offset); // gets offset
	type_info->get_name (type_info, &name);
	type_info->get_print_type (type_info, &type);
	RAnalStructMember *member = R_NEW0 (RAnalStructMember);
	if (!member) {
		goto cleanup;
	}
	char *sname = r_str_sanitize_sdb_key (name);
	member->name = sname;
	member->type = strdup (type); // we assume it's sanitized
	member->offset = offset;
	return member;
cleanup:
	return NULL;
}
static RAnalEnumCase *parse_enumerate(STypeInfo *type_info, RList *types) {
	r_return_val_if_fail (type_info && types && type_info->leaf_type == eLF_ENUMERATE, NULL);
	r_return_val_if_fail (type_info->get_val && type_info->get_name, NULL);

	char *name = NULL;
	int value = 0;
	// sometimes, the type doesn't have get_val for some reason
	type_info->get_val (type_info, &value);
	type_info->get_name (type_info, &name);
	RAnalEnumCase *cas = R_NEW0 (RAnalEnumCase);
	if (!cas) {
		goto cleanup;
	}
	char *sname = r_str_sanitize_sdb_key (name);
	cas->name = sname;
	cas->val = value;
	return cas;
cleanup:
	return NULL;
}

static void parse_enum(const RAnal *anal, SType *type, RList *types) {
	r_return_if_fail (anal && type && types);
	STypeInfo *type_info = &type->type_data;
	// assert all member functions we need info from
	r_return_if_fail (type_info->get_members &&
		type_info->get_name &&
		type_info->get_utype);

	RAnalBaseType *base_type = r_anal_new_base_type (R_ANAL_BASE_TYPE_KIND_ENUM);
	if (!base_type) {
		return;
	}

	char *name = NULL;
	type_info->get_name (type_info, &name);
	bool to_free_name = false;
	if (!name) {
		name = create_type_name_from_offset (type->tpi_idx);
		to_free_name = true;
	}
	type_info->get_utype (type_info, (void **)&type);
	int size = 0;
	char *type_name = NULL;
	if (type && type->type_data.type_info) {
		SLF_SIMPLE_TYPE *base_type = type->type_data.type_info;
		type_name = base_type->type;
		size = base_type->size;
	}
	RList *members;
	type_info->get_members (type_info, &members);

	r_vector_init (&base_type->enum_data.cases,
		sizeof (RAnalEnumCase), enum_type_fini, NULL);

	RListIter *it = r_list_iterator (members);
	while (r_list_iter_next (it)) {
		STypeInfo *member_info = r_list_iter_get (it);
		RAnalEnumCase *enum_case = parse_enumerate (member_info, types);
		if (!enum_case) {
			continue; // skip it, move forward
		}
		void *element = r_vector_push (&base_type->struct_data.members, enum_case);
		if (!element) {
			goto cleanup;
		}
	}
	char *sname = r_str_sanitize_sdb_key (name);
	base_type->name = sname;
	base_type->size = size;
	base_type->type = strdup (type_name); // we assume it's sanitized

	r_anal_save_base_type (anal, base_type);
cleanup:
	if (to_free_name) {
		R_FREE (name);
	}
	r_anal_free_base_type (base_type);
	return;
}

// parses classes, unions and structures for now
static void parse_structure(const RAnal *anal, SType *type, RList *types) {
	r_return_if_fail (anal && type && types);
	STypeInfo *type_info = &type->type_data;
	// assert all member functions we need info from
	r_return_if_fail (type_info->get_members &&
		type_info->is_fwdref &&
		type_info->get_name &&
		type_info->get_val);

	RAnalBaseType *base_type = r_anal_new_base_type (R_ANAL_BASE_TYPE_KIND_STRUCT);
	if (!base_type) {
		return;
	}
	r_vector_init (&base_type->struct_data.members,
		sizeof (RAnalStructMember), struct_type_fini, NULL);

	char *name = NULL;
	type_info->get_name (type_info, &name);
	bool to_free_name = false;
	if (!name) {
		name = create_type_name_from_offset (type->tpi_idx);
		to_free_name = true;
	}
	int size;
	type_info->get_val (type_info, &size); // gets size

	RList *members;
	type_info->get_members (type_info, &members);

	RListIter *it = r_list_iterator (members);
	while (r_list_iter_next (it)) {
		STypeInfo *member_info = r_list_iter_get (it);
		RAnalStructMember *struct_member = parse_member (member_info, types);
		if (!struct_member) {
			continue; // skip the failure
		}
		void *element = r_vector_push (&base_type->struct_data.members, struct_member);
		if (!element) {
			goto cleanup;
		}
	}
	if (type_info->leaf_type == eLF_STRUCTURE || type_info->leaf_type == eLF_CLASS) {
		base_type->kind = R_ANAL_BASE_TYPE_KIND_STRUCT;
	} else { // union
		base_type->kind = R_ANAL_BASE_TYPE_KIND_UNION;
	}
	char *sname = r_str_sanitize_sdb_key (name);
	base_type->name = sname;
	base_type->size = size;
	r_anal_save_base_type (anal, base_type);
cleanup:
	if (to_free_name) {
		R_FREE (name);
	}
	r_anal_free_base_type (base_type);
	return;
}

static void parse_type (const RAnal *anal, SType *type, RList *types) {
	r_return_if_fail (anal && type && types);

	int is_forward_decl;
	if (type->type_data.is_fwdref) {
		type->type_data.is_fwdref (&type->type_data, &is_forward_decl);
		if (is_forward_decl) { // we skip those, atleast for now
			return;
		}
	}
	switch (type->type_data.leaf_type) {
	case eLF_CLASS:
	case eLF_STRUCTURE:
	case eLF_UNION:
		parse_structure (anal, type, types);
		break;
	case eLF_ENUM:
		parse_enum (anal, type, types);
		break;
	default:
		eprintf ("Unknown type record");
		break;
	}
}

/**
 * @brief Saves PDB types from TPI stream into the SDB
 * 
 * @param anal 
 * @param pdb 
 */
R_API void parse_pdb_types(const RAnal *anal, const R_PDB *pdb) {
	r_return_if_fail (anal && pdb);
	RList *plist = pdb->pdb_streams;
	// getting the TPI stream from the streams list
	STpiStream *tpi_stream = r_list_get_n (plist, ePDB_STREAM_TPI);
	if (!tpi_stream) { // no TPI stream found
		return;
	}
	// Types should be DAC - only references previous records
	RListIter *iter = r_list_iterator (tpi_stream->types);
	while (r_list_iter_next (iter)) { // iterate all types
		SType *type = r_list_iter_get (iter);
		if (is_parsable_type (type->type_data.leaf_type)) {
			parse_type (anal, type, tpi_stream->types);
		}
	}
}