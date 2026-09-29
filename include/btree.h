#ifndef BTREE_H
#define BTREE_H

#include<stdbool.h>
#include<stdint.h>

#include "pager.h"
#include "record.h"
#include "schema.h"

/*
 * Node kinds in the b-tree. Leaf nodes hold the actual cells (key + serialized
 * row). Internal nodes hold only routing information: child page numbers
 * separated by keys, where each key is the largest key found in the child to
 * its left.
 *
 * The kind is stored in the node's header byte, so a page can describe itself:
 * callers dispatch on get_node_type() rather than assuming what a page holds.
 */
typedef enum{
    NODE_INTERNAL,
    NODE_LEAF
}NodeType;

/* Reads the node's kind from its header. */
NodeType get_node_type(void* node);

/* Stamps the node's kind into its header, as a single byte. */
void set_node_type(void* node, NodeType type);

/* Whether this node is the root of its tree. */
bool is_node_root(void* node);

/* Marks or unmarks the node as the root of its tree. */
void set_node_root(void* node, bool is_root);

/*
 * Leaf nodes. Every node occupies exactly one page. A leaf's cell size depends
 * on the table's row width, so capacity and field addresses are functions of
 * the Schema rather than compile-time constants. The accessors return live
 * pointers into the page, usable as both getters and setters.
 */

/* Maximum cells a leaf can hold, given the schema's row size. */
uint32_t leaf_node_max_cells(const Schema* schema);

/* Pointer to the leaf's cell-count field. */
uint32_t* leaf_node_num_cells(void* node);

/*
 * Pointer to the page number of the leaf's right sibling, or 0 for the
 * rightmost leaf. Chaining leaves this way lets a scan walk every row in key
 * order without going back up through the internal nodes.
 */
uint32_t* leaf_node_next_leaf(void* node);

/* Pointer to the key of cell `cell_num`. */
uint32_t* leaf_node_key(void* node, uint32_t cell_num, const Schema* schema);

/* Pointer to the value (serialized row) of cell `cell_num`. */
void* leaf_node_value(void* node, uint32_t cell_num, const Schema* schema);

/*
 * Binary-searches a leaf's sorted cells for `key`. Returns the index of the
 * matching cell if present, otherwise the index the key belongs at — which may
 * be the cell count itself, meaning "past the last cell".
 */
uint32_t leaf_node_find_cell(void* node, uint32_t key, const Schema* schema);

/* Turns a fresh page into an empty, non-root leaf node. */
void initialize_leaf_node(void* node);

/*
 * Inserts a key/row cell at `cell_num` of the leaf on page `page_num`, shifting
 * later cells right. A full leaf is split in two instead: if that leaf was the
 * root, a new internal root is created above both halves; otherwise its parent
 * gets an updated separator and a new child for the upper half.
 */
void leaf_node_insert(Pager* pager, uint32_t page_num, uint32_t cell_num, uint32_t key,
                      const Record* value, const Schema* schema);

/*
 * Internal nodes. The header holds the key count and the rightmost child; the
 * body is an array of (child page, key) cells. A node with N keys therefore has
 * N + 1 children, the last of which lives in the header rather than a cell.
 */

/* Pointer to the internal node's key-count field. */
uint32_t* internal_node_num_keys(void* node);

/* Pointer to the page number of the rightmost child. */
uint32_t* internal_node_right_child(void* node);

/*
 * Pointer to the page number of child `child_num`, valid for
 * 0 <= child_num <= num_keys. Exits on an out-of-range index.
 */
uint32_t* internal_node_child(void* node, uint32_t child_num);

/* Pointer to key `key_num`: the largest key in the child to its left. */
uint32_t* internal_node_key(void* node, uint32_t key_num);

/*
 * Binary-searches an internal node's separator keys for the child whose
 * subtree owns `key`. Returns a child index in [0, num_keys], where num_keys
 * means the rightmost child; pass it to internal_node_child for the page.
 */
uint32_t internal_node_find_child(void* node, uint32_t key);

/* Prints the layout constants (used by the .constants meta command). */
void print_constants(const Schema* schema);

/*
 * Prints the subtree rooted at `page_num`, one node per line and indented by
 * depth (used by the .btree meta command). Works on a tree of any height.
 */
void print_tree(Pager* pager, const Schema* schema, uint32_t page_num, uint32_t indentation_level);

#endif
