//
//  list.c
//  pscal
//
//  Created by Michael Miller on 3/8/25.
//

// This file implements a simple singly linked list data structure
// for storing dynamically allocated strings. It supports basic operations
// like creating a list, appending elements, getting the size,
// retrieving an element by index, checking for containment, and freeing the list.

#include "list.h"
#include "Pascal/globals.h" // For EXIT_FAILURE_HANDLER
#include <stdlib.h>  // For malloc, free, EXIT_FAILURE
#include <string.h>  // For strdup, strcmp, strcasecmp
#include <stdio.h>   // For fprintf, stderr
#include <stdbool.h> // For bool type

// --- ListNode Structure (defined in list.h) ---
// struct ListNode {
//     char *value;        // The string value stored in this node (dynamically allocated copy)
//     struct ListNode *next; // Pointer to the next node in the list
// };

// --- List Structure (defined in list.h) ---
// typedef struct {
//     ListNode *head; // Pointer to the first node in the list
//     ListNode *tail; // Pointer to the last node in the list
//     int size;       // Current number of nodes in the list
// } List;


/**
 * Creates a new empty linked list.
 * Allocates memory for the List structure and initializes its fields.
 * Exits the program with a failure status if memory allocation fails.
 *
 * @return A pointer to the newly created List structure.
 */
List *createList(void) {
    // Allocate memory for the List control structure.
    List *list = malloc(sizeof(List));

    // Check if memory allocation failed.
    if (!list) {
        // Print an error message to standard error.
        fprintf(stderr, "Memory allocation error in createList\n");
        // Terminate the program with a failure status.
        EXIT_FAILURE_HANDLER();
    }

    // Initialize the list to be empty.
    list->head = NULL; // Head points to nothing initially
    list->tail = NULL; // Tail points to nothing initially
    list->size = 0;    // Size is zero for an empty list

    // Return the pointer to the newly created list.
    return list;
}

/**
 * Appends a new element with the given string value to the end of the list.
 * Creates a deep copy of the input string value.
 * Exits the program with a failure status if memory allocation or string duplication fails.
 *
 * @param list  A pointer to the List structure.
 * @param value The string value to append. A deep copy is made.
 */
void listAppend(List *list, const char *value) {
    // Allocate memory for a new list node.
    ListNode *node = malloc(sizeof(ListNode));

    // Check if memory allocation for the node failed.
    if (!node) {
        fprintf(stderr, "Memory allocation error in listAppend (node)\n");
        EXIT_FAILURE_HANDLER();
    }

    // Create a deep copy of the input string value using strdup.
    // strdup allocates memory for the string and copies the content.
    node->value = strdup(value);

    // Check if string duplication failed.
    if (!node->value) {
        fprintf(stderr, "Memory allocation error in listAppend (value strdup)\n");
        free(node); // Free the node struct itself before exiting
        EXIT_FAILURE_HANDLER();
    }

    node->next = NULL; // The new node is the last, so its 'next' is NULL

    // Link the new node into the list.
    if (list->tail) {
        // If the list is not empty, link the current tail's 'next' to the new node.
        list->tail->next = node;
    } else {
        // If the list was empty, the new node is both the head and the tail.
        list->head = node;
    }

    list->tail = node; // The new node is always the new tail of the list.
    list->size++;      // Increment the list size.
}

/**
 * Returns the current number of elements in the list.
 * This is an O(1) operation.
 *
 * @param list A pointer to the List structure.
 * @return The number of elements in the list.
 */
int listSize(const List *list) {
    // Return the stored size directly.
    return list->size;
}

/**
 * Retrieves the string value of the element at the specified index.
 * Traverses the list from the head. This is an O(index) operation.
 * Exits the program with a failure status if the index is out of bounds.
 *
 * IMPORTANT: The returned char* points to memory owned by the list node.
 * The caller should NOT modify or free this string.
 *
 * @param list  A pointer to the List structure.
 * @param index The zero-based index of the element to retrieve.
 * @return A pointer to the string value at the specified index.
 */
char *listGet(const List *list, int index) {
    // Check if the provided index is within the valid range [0, size - 1].
    if (index < 0 || index >= list->size) {
        fprintf(stderr, "Index out of bounds in listGet: index=%d, size=%d\n", index, list->size);
        EXIT_FAILURE_HANDLER();
    }

    // Start traversal from the head of the list.
    ListNode *current = list->head;

    // Traverse the list until the desired index is reached.
    for (int i = 0; i < index; i++) {
        // Since index is guaranteed to be valid, current->next will not be NULL here.
        current = current->next;
    }

    // Return the string value of the node at the specified index.
    // The caller does NOT own this memory and should not free it.
    return current->value;
}

/**
 * Frees all memory allocated for the list, including all nodes and their string values.
 * After this function is called, the List pointer should be considered invalid.
 *
 * @param list A pointer to the List structure to free.
 */
void freeList(List *list) {
    // Return immediately if the list pointer is NULL.
    if (!list) {
        return;
    }

    // Start from the head of the list.
    ListNode *current = list->head;

    // Traverse the list, freeing each node.
    while (current) {
        // Store the pointer to the next node before freeing the current node.
        ListNode *temp = current;
        current = current->next;

        // Free the dynamically allocated string value within the node.
        // strdup was used in listAppend, so free is required here.
        if (temp->value) { // Defensive check, though strdup shouldn't return NULL on success
            free(temp->value);
            temp->value = NULL; // Avoid dangling pointer
        }

        // Free the ListNode structure itself.
        free(temp);
    }

    // Free the List control structure.
    free(list);
    // Note: The caller should set their pointer to NULL after calling freeList to avoid a dangling pointer.
}

/**
 * Checks if the list contains an element with the given string value (case-insensitive).
 * Traverses the list from the head. This is an O(n) operation in the worst case.
 *
 * @param list  A pointer to the List structure.
 * @param value The string value to search for.
 * @return true if the value is found in the list, false otherwise.
 */
bool listContains(const List *list, const char *value) {
    // Return false immediately if the list pointer or the value to search is NULL.
    if (!list || !value) {
        return false;
    }

    // Start traversal from the head.
    ListNode *current = list->head;

    // Traverse the list.
    while (current) {
        // Perform case-insensitive comparison of the stored value with the target value.
        // Ensure current->value is not NULL before calling strcasecmp.
        if (current->value && strcasecmp(current->value, value) == 0) {
            // Value found.
            return true;
        }
        // Move to the next node.
        current = current->next;
    }

    // Value not found after traversing the entire list.
    return false;
}
