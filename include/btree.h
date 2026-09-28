#ifndef BTREE_H
#define BTREE_H

#include<stdint.h>

#include "record.h"
#include "schema.h"

/*
 * Node kinds in the b-tree. Internal nodes route by key and point at children;
 * leaf nodes hold the actual cells (key + serialized row). Only leaf nodes
 * exist so far — this module gains internal-node support in later parts.
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

/*
 * Every node occupies exactly one page. A leaf node's cell size depends on the
 * table's row width, so capacity and field addresses are functions of the
 * Schema rather than compile-time constants. The accessors return live
 * pointers into the page, usable as both getters and setters.
 */

/* Maximum cells a leaf can hold, given the schema's row size. */
uint32_t leaf_node_max_cells(const Schema* schema);

/* Pointer to the leaf's cell-count field. */
uint32_t* leaf_node_num_cells(void* node);

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

/* Turns a fresh page into an empty leaf node (typed, cell count = 0). */
void initialize_leaf_node(void* node);

/*
 * Inserts a key/row cell at `cell_num`, shifting later cells right. Exits if
 * the node is already full (splitting is not implemented yet).
 */
void leaf_node_insert(void* node, uint32_t cell_num, uint32_t key,
                      const Record* value, const Schema* schema);

/* Prints the layout constants (used by the .constants meta command). */
void print_constants(const Schema* schema);

/* Prints a leaf node's cell count and keys (used by the .btree meta command). */
void print_leaf_node(void* node, const Schema* schema);

#endif
