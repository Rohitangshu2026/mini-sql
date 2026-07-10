#ifndef BTREE_H
#define BTREE_H

#include<stdint.h>

#include "record.h"
#include "schema.h"

/*
 * Node kinds in the b-tree. Internal nodes route by key and point at children;
 * leaf nodes hold the actual cells (key + serialized row). Only leaf nodes
 * exist so far — this module gains internal-node support in later parts.
 */
typedef enum{
    NODE_INTERNAL,
    NODE_LEAF
}NodeType;

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

/* Turns a fresh page into an empty leaf node (cell count = 0). */
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
