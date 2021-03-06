/*
 * Copyright (c) 2013 Hugh Bailey <obs.jim@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <inttypes.h>
#include <stdio.h>
#include <wchar.h>
#include "config-file.h"
#include "platform.h"
#include "base.h"
#include "bmem.h"
#include "darray.h"
#include "lexer.h"
#include "dstr.h"

#include <jansson.h>

struct config_item {
	char *name;
	char *value;
};

static inline void config_item_free(struct config_item *item)
{
	bfree(item->name);
	bfree(item->value);
}

struct config_section {
	char *name;
	struct darray items; /* struct config_item */
};

static inline void config_section_free(struct config_section *section)
{
	struct config_item *items = section->items.array;
	size_t i;

	for (i = 0; i < section->items.num; i++)
		config_item_free(items+i);

	darray_free(&section->items);
	bfree(section->name);
}

struct config_data {
	char *file;
	struct darray sections; /* struct config_section */
	struct darray defaults; /* struct config_section */
};

config_t *config_create(const char *file)
{
	struct config_data *config;
	FILE *f;

	f = os_fopen(file, "wb");
	if (!f)
		return NULL;
	fclose(f);

	config = bzalloc(sizeof(struct config_data));
	return config;
}

static inline void remove_ref_whitespace(struct strref *ref)
{
	if (ref->array) {
		while (is_whitespace(*ref->array)) {
			ref->array++;
			ref->len--;
		}

		while (ref->len && is_whitespace(ref->array[ref->len-1]))
			ref->len--;
	}
}

static bool config_parse_string(struct lexer *lex, struct strref *ref,
		char end)
{
	bool success = end != 0;
	struct base_token token;
	base_token_clear(&token);

	while (lexer_getbasetoken(lex, &token, PARSE_WHITESPACE)) {
		if (end) {
			if (*token.text.array == end) {
				success = true;
				break;
			} else if (is_newline(*token.text.array)) {
				success = false;
				break;
			}
		} else {
			if (is_newline(*token.text.array)) {
				success = true;
				break;
			}
		}

		strref_add(ref, &token.text);
	}

	remove_ref_whitespace(ref);
	return success;
}

static void config_add_item(struct darray *items, struct strref *name,
		struct strref *value)
{
	struct config_item item;
	struct dstr item_value;
	dstr_init_copy_strref(&item_value, value);
	dstr_replace(&item_value, "\\n", "\n");
	dstr_replace(&item_value, "\\r", "\r");
	dstr_replace(&item_value, "\\\\", "\\");

	item.name  = bstrdup_n(name->array,  name->len);
	item.value = item_value.array;
	darray_push_back(sizeof(struct config_item), items, &item);
}

static void config_parse_section(struct config_section *section,
		struct lexer *lex)
{
	struct base_token token;

	while (lexer_getbasetoken(lex, &token, PARSE_WHITESPACE)) {
		struct strref name, value;

		while (token.type == BASETOKEN_WHITESPACE) {
			if (!lexer_getbasetoken(lex, &token, PARSE_WHITESPACE))
				return;
		}

		if (token.type == BASETOKEN_OTHER) {
			if (*token.text.array == '#') {
				do {
					if (!lexer_getbasetoken(lex, &token,
							PARSE_WHITESPACE))
						return;
				} while (!is_newline(*token.text.array));

				continue;
			} else if (*token.text.array == '[') {
				lex->offset--;
				return;
			}
		}

		strref_copy(&name, &token.text);
		if (!config_parse_string(lex, &name, '='))
			continue;

		strref_clear(&value);
		config_parse_string(lex, &value, 0);

		if (!strref_is_empty(&value))
			config_add_item(&section->items, &name, &value);
	}
}

void parse_json_config_data(struct darray *sections, struct lexer *lex);
static void parse_config_data(struct darray *sections, struct lexer *lex)
{
	struct strref section_name;
	struct base_token token;

	if (lex->text[0] == '{')
	{
		parse_json_config_data(sections, lex);
		return;
	}

	base_token_clear(&token);

	while (lexer_getbasetoken(lex, &token, PARSE_WHITESPACE)) {
		struct config_section *section;

		while (token.type == BASETOKEN_WHITESPACE) {
			if (!lexer_getbasetoken(lex, &token, PARSE_WHITESPACE))
				return;
		}

		if (*token.text.array != '[') {
			while (!is_newline(*token.text.array)) {
				if (!lexer_getbasetoken(lex, &token,
							PARSE_WHITESPACE))
					return;
			}

			continue;
		}

		strref_clear(&section_name);
		config_parse_string(lex, &section_name, ']');
		if (!section_name.len)
			return;

		section = darray_push_back_new(sizeof(struct config_section),
				sections);
		section->name = bstrdup_n(section_name.array,
				section_name.len);
		config_parse_section(section, lex);
	}
}

static void parse_json_config_data(struct darray *sections, struct lexer *lex)
{
	json_error_t jsonError;

	json_t* sectionArray;

	const char* sectionName;
	json_t* itemArray;

	const char* itemKeyName;
	json_t* itemNode;

	sectionArray = json_loads(lex->text, 0, &jsonError);

	json_object_foreach(sectionArray, sectionName, itemArray)
	{
		struct config_section *section =
			darray_push_back_new(sizeof(struct config_section),
			sections);
		section->name = bstrdup_n(sectionName, strlen(sectionName));

		json_object_foreach(itemArray, itemKeyName, itemNode)
		{
			struct config_item item;
			const char* itemValue;
			item.name = bstrdup_n(itemKeyName, strlen(itemKeyName));
			itemValue = json_string_value(itemNode);
			item.value = bstrdup_n(itemValue, strlen(itemValue));
			darray_push_back(sizeof(item), &section->items, &item);
		}
	}

	json_decref(sectionArray);
}

static int config_parse_file(struct darray *sections, const char *file,
		bool always_open)
{
	char *file_data;
	struct lexer lex;
	FILE *f;

	f = os_fopen(file, "rb");
	if (always_open && !f)
		f = os_fopen(file, "w+");
	if (!f)
		return CONFIG_FILENOTFOUND;

	os_fread_utf8(f, &file_data);
	fclose(f);

	if (!file_data)
		return CONFIG_SUCCESS;

	lexer_init(&lex);
	lexer_start_move(&lex, file_data);

	parse_config_data(sections, &lex);

	lexer_free(&lex);
	return CONFIG_SUCCESS;
}

int config_open(config_t **config, const char *file,
		enum config_open_type open_type)
{
	int errorcode;
	bool always_open = open_type == CONFIG_OPEN_ALWAYS;

	if (!config)
		return CONFIG_ERROR;

	*config = bzalloc(sizeof(struct config_data));
	if (!*config)
		return CONFIG_ERROR;

	(*config)->file = bstrdup(file);

	errorcode = config_parse_file(&(*config)->sections, file, always_open);

	if (errorcode != CONFIG_SUCCESS) {
		config_close(*config);
		*config = NULL;
	}

	return errorcode;
}

int config_open_string(config_t **config, const char *str)
{
	struct lexer lex;

	if (!config)
		return CONFIG_ERROR;

	*config = bzalloc(sizeof(struct config_data));
	if (!*config)
		return CONFIG_ERROR;

	(*config)->file = NULL;

	lexer_init(&lex);
	lexer_start(&lex, str);
	parse_config_data(&(*config)->sections, &lex);
	lexer_free(&lex);

	return CONFIG_SUCCESS;
}

int config_open_defaults(config_t *config, const char *file)
{
	if (!config)
		return CONFIG_ERROR;

	return config_parse_file(&config->defaults, file, false);
}

int config_save_json(config_t *config);
int config_save(config_t *config)
{
	return config_save_json(config);
#if 0
	FILE *f;
	struct dstr str, tmp;
	size_t i, j;

	if (!config)
		return CONFIG_ERROR;
	if (!config->file)
		return CONFIG_ERROR;

	dstr_init(&str);
	dstr_init(&tmp);

	f = os_fopen(config->file, "wb");
	if (!f)
		return CONFIG_FILENOTFOUND;

	for (i = 0; i < config->sections.num; i++) {
		struct config_section *section = darray_item(
				sizeof(struct config_section),
				&config->sections, i);

		if (i) dstr_cat(&str, "\n");

		dstr_cat(&str, "[");
		dstr_cat(&str, section->name);
		dstr_cat(&str, "]\n");

		for (j = 0; j < section->items.num; j++) {
			struct config_item *item = darray_item(
					sizeof(struct config_item),
					&section->items, j);

			dstr_copy(&tmp, item->value ? item->value : "");
			dstr_replace(&tmp, "\\", "\\\\");
			dstr_replace(&tmp, "\r", "\\r");
			dstr_replace(&tmp, "\n", "\\n");

			dstr_cat(&str, item->name);
			dstr_cat(&str, "=");
			dstr_cat(&str, tmp.array);
			dstr_cat(&str, "\n");
		}
	}

#ifdef _WIN32
	fwrite("\xEF\xBB\xBF", 1, 3, f);
#endif
	fwrite(str.array, 1, str.len, f);
	fclose(f);

	dstr_free(&tmp);
	dstr_free(&str);

	return CONFIG_SUCCESS;
#endif
}

int config_save_json(config_t *config)
{
	FILE *f;
	size_t i, j;

	char* jsonString;

	json_t* sectionArray;
	json_t* itemArray;

	//json根节点
	sectionArray = json_object();

	if (!config)
		return CONFIG_ERROR;
	if (!config->file)
		return CONFIG_ERROR;

	//json每个section
	for (i = 0; i < config->sections.num; ++i)
	{
		itemArray = json_object();
		struct config_section* section = darray_item(
			sizeof(*section),
			&config->sections,
			i);

		//json的每个section里的item
		for (j = 0; j < section->items.num; ++j)
		{
			struct config_item* item = darray_item(
				sizeof(*item),
				&section->items,
				j);

			json_object_set(itemArray, item->name, json_string(item->value));
		}

		json_object_set(sectionArray, section->name, itemArray);
		json_decref(itemArray);
	}
	jsonString = json_dumps(sectionArray, JSON_INDENT(4));
	json_decref(sectionArray);

	f = os_fopen(config->file, "wb");
	if (!f)
		return CONFIG_FILENOTFOUND;

#ifdef _WIN32
	fwrite("\xEF\xBB\xBF", 1, 3, f);
#endif
	fwrite(jsonString, 1, strlen(jsonString), f);
	fclose(f);

	{
		json_free_t jsonFreeFn;
		json_get_alloc_funcs(0, &jsonFreeFn);
		(*jsonFreeFn)(jsonString);
	}

	return CONFIG_SUCCESS;
}

int config_save_safe(config_t *config, const char *temp_ext,
		const char *backup_ext)
{
	struct dstr temp_file = {0};
	struct dstr backup_file = {0};
	char *file = config->file;
	int ret;

	if (!temp_ext || !*temp_ext) {
		blog(LOG_ERROR, "config_save_safe: invalid "
		                "temporary extension specified");
		return CONFIG_ERROR;
	}

	dstr_copy(&temp_file, config->file);
	if (*temp_ext != '.')
		dstr_cat(&temp_file, ".");
	dstr_cat(&temp_file, temp_ext);

	config->file = temp_file.array;
	ret = config_save(config);
	config->file = file;

	if (ret != CONFIG_SUCCESS) {
		goto cleanup;
	}

	if (backup_ext && *backup_ext) {
		dstr_copy(&backup_file, config->file);
		if (*backup_ext != '.')
			dstr_cat(&backup_file, ".");
		dstr_cat(&backup_file, backup_ext);

		os_unlink(backup_file.array);
		os_rename(file, backup_file.array);
	} else {
		os_unlink(file);
	}

	os_rename(temp_file.array, file);

cleanup:
	dstr_free(&temp_file);
	dstr_free(&backup_file);
	return ret;
}

void config_close(config_t *config)
{
	struct config_section *defaults, *sections;
	size_t i;

	if (!config) return;

	defaults = config->defaults.array;
	sections = config->sections.array;

	for (i = 0; i < config->defaults.num; i++)
		config_section_free(defaults+i);
	for (i = 0; i < config->sections.num; i++)
		config_section_free(sections+i);

	darray_free(&config->defaults);
	darray_free(&config->sections);
	bfree(config->file);
	bfree(config);
}

size_t config_num_sections(config_t *config)
{
	return config->sections.num;
}

const char *config_get_section(config_t *config, size_t idx)
{
	struct config_section *section;

	if (idx >= config->sections.num)
		return NULL;

	section = darray_item(sizeof(struct config_section), &config->sections,
			idx);

	return section->name;
}

static const struct config_item *config_find_item(const struct darray *sections,
		const char *section, const char *name)
{
	size_t i, j;

	for (i = 0; i < sections->num; i++) {
		const struct config_section *sec = darray_item(
				sizeof(struct config_section), sections, i);

		if (astrcmpi(sec->name, section) == 0) {
			for (j = 0; j < sec->items.num; j++) {
				struct config_item *item = darray_item(
						sizeof(struct config_item),
						&sec->items, j);

				if (astrcmpi(item->name, name) == 0)
					return item;
			}
		}
	}

	return NULL;
}

static void config_set_item(struct darray *sections, const char *section,
		const char *name, char *value)
{
	struct config_section *sec = NULL;
	struct config_section *array = sections->array;
	struct config_item *item;
	size_t i, j;

	for (i = 0; i < sections->num; i++) {
		struct config_section *cur_sec = array+i;
		struct config_item *items = cur_sec->items.array;

		if (astrcmpi(cur_sec->name, section) == 0) {
			for (j = 0; j < cur_sec->items.num; j++) {
				item = items+j;

				if (astrcmpi(item->name, name) == 0) {
					bfree(item->value);
					item->value = value;
					return;
				}
			}

			sec = cur_sec;
			break;
		}
	}

	if (!sec) {
		sec = darray_push_back_new(sizeof(struct config_section),
				sections);
		sec->name = bstrdup(section);
	}

	item = darray_push_back_new(sizeof(struct config_item), &sec->items);
	item->name  = bstrdup(name);
	item->value = value;
}

void config_set_string(config_t *config, const char *section,
		const char *name, const char *value)
{
	if (!value)
		value = "";
	config_set_item(&config->sections, section, name, bstrdup(value));
}

void config_set_int(config_t *config, const char *section,
		const char *name, int64_t value)
{
	struct dstr str;
	dstr_init(&str);
	dstr_printf(&str, "%"PRId64, value);
	config_set_item(&config->sections, section, name, str.array);
}

void config_set_uint(config_t *config, const char *section,
		const char *name, uint64_t value)
{
	struct dstr str;
	dstr_init(&str);
	dstr_printf(&str, "%"PRIu64, value);
	config_set_item(&config->sections, section, name, str.array);
}

void config_set_bool(config_t *config, const char *section,
		const char *name, bool value)
{
	char *str = bstrdup(value ? "true" : "false");
	config_set_item(&config->sections, section, name, str);
}

void config_set_double(config_t *config, const char *section,
		const char *name, double value)
{
	char *str = bzalloc(64);
	os_dtostr(value, str, 64);
	config_set_item(&config->sections, section, name, str);
}

void config_set_default_string(config_t *config, const char *section,
		const char *name, const char *value)
{
	if (!value)
		value = "";
	config_set_item(&config->defaults, section, name, bstrdup(value));
}

void config_set_default_int(config_t *config, const char *section,
		const char *name, int64_t value)
{
	struct dstr str;
	dstr_init(&str);
	dstr_printf(&str, "%"PRId64, value);
	config_set_item(&config->defaults, section, name, str.array);
}

void config_set_default_uint(config_t *config, const char *section,
		const char *name, uint64_t value)
{
	struct dstr str;
	dstr_init(&str);
	dstr_printf(&str, "%"PRIu64, value);
	config_set_item(&config->defaults, section, name, str.array);
}

void config_set_default_bool(config_t *config, const char *section,
		const char *name, bool value)
{
	char *str = bstrdup(value ? "true" : "false");
	config_set_item(&config->defaults, section, name, str);
}

void config_set_default_double(config_t *config, const char *section,
		const char *name, double value)
{
	struct dstr str;
	dstr_init(&str);
	dstr_printf(&str, "%g", value);
	config_set_item(&config->defaults, section, name, str.array);
}

const char *config_get_string(const config_t *config, const char *section,
		const char *name)
{
	const struct config_item *item = config_find_item(&config->sections,
			section, name);
	if (!item)
		item = config_find_item(&config->defaults, section, name);
	if (!item)
		return NULL;

	return item->value;
}

static inline int64_t str_to_int64(const char *str)
{
	if (!str || !*str)
		return 0;

	if (str[0] == '0' && str[1] == 'x')
		return strtoll(str + 2, NULL, 16);
	else
		return strtoll(str, NULL, 10);
}

static inline uint64_t str_to_uint64(const char *str)
{
	if (!str || !*str)
		return 0;

	if (str[0] == '0' && str[1] == 'x')
		return strtoull(str + 2, NULL, 16);
	else
		return strtoull(str, NULL, 10);
}

int64_t config_get_int(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_string(config, section, name);
	if (value)
		return str_to_int64(value);

	return 0;
}

uint64_t config_get_uint(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_string(config, section, name);
	if (value)
		return str_to_uint64(value);

	return 0;
}

bool config_get_bool(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_string(config, section, name);
	if (value)
		return astrcmpi(value, "true") == 0 ||
		       !!str_to_uint64(value);

	return false;
}

double config_get_double(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_string(config, section, name);
	if (value)
		return os_strtod(value);

	return 0.0;
}

bool config_remove_value(config_t *config, const char *section,
		const char *name)
{
	struct darray *sections = &config->sections;

	for (size_t i = 0; i < sections->num; i++) {
		struct config_section *sec = darray_item(
				sizeof(struct config_section), sections, i);

		if (astrcmpi(sec->name, section) != 0)
			continue;

		for (size_t j = 0; j < sec->items.num; j++) {
			struct config_item *item = darray_item(
					sizeof(struct config_item),
					&sec->items, j);

			if (astrcmpi(item->name, name) == 0) {
				config_item_free(item);
				darray_erase(sizeof(struct config_item),
						&sec->items, j);
				return true;
			}
		}
	}

	return false;
}

const char *config_get_default_string(const config_t *config,
		const char *section, const char *name)
{
	const struct config_item *item;

	item = config_find_item(&config->defaults, section, name);
	if (!item)
		return NULL;

	return item->value;
}

int64_t config_get_default_int(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_default_string(config, section, name);
	if (value)
		return str_to_int64(value);

	return 0;
}

uint64_t config_get_default_uint(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_default_string(config, section, name);
	if (value)
		return str_to_uint64(value);

	return 0;
}

bool config_get_default_bool(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_default_string(config, section, name);
	if (value)
		return astrcmpi(value, "true") == 0 ||
		       !!str_to_uint64(value);

	return false;
}

double config_get_default_double(const config_t *config, const char *section,
		const char *name)
{
	const char *value = config_get_default_string(config, section, name);
	if (value)
		return os_strtod(value);

	return 0.0;
}

bool config_has_user_value(const config_t *config, const char *section,
		const char *name)
{
	return config_find_item(&config->sections, section, name) != NULL;
}

bool config_has_default_value(const config_t *config, const char *section,
		const char *name)
{
	return config_find_item(&config->defaults, section, name) != NULL;
}

