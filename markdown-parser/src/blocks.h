#ifndef MD_BLOCKS_H
#define MD_BLOCKS_H

/*
 * Block-level parser. md_parse_blocks() is the entry point declared in md.h;
 * md_parse_blocks_into() parses `lines` into an existing container node and is
 * used for the recursive descent into block quotes and list items.
 */

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include "status.h"

#include <stddef.h>

/* Guard against unbounded recursion on pathological input. */
#define MD_MAX_NESTING 64u

/* Parse every line of `lines` as block content of `parent`. */
md_status md_parse_blocks_into(md_arena *arena, const md_lines *lines,
                               md_node *parent, unsigned depth);

#endif
