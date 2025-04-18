//
//  list.c
//  pscal
//
//  Created by Michael Miller on 3/8/25.
//
#include "list.h"
#include "globals.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

List *createList(void) {
    List *list = malloc(sizeof(List));
    if (!list) {
        fprintf(stderr, "Memory allocation error in create_list\n");
        EXIT_FAILURE_HANDLER();
    }
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
    return list;
}

void listAppend(List *list, const char *value) {
    ListNode *node = malloc(sizeof(ListNode));
    if (!node) {
        fprintf(stderr, "Memory allocation error in list_append\n");
        EXIT_FAILURE_HANDLER();
    }
    node->value = strdup(value);
    node->next = NULL;
    if (list->tail) {
        list->tail->next = node;
    } else {
        list->head = node;
    }
    list->tail = node;
    list->size++;
}

int listSize(const List *list) {
    return list->size;
}

char *listGet(const List *list, int index) {
    if (index < 0 || index >= list->size) {
        fprintf(stderr, "Index out of bounds in list_get\n");
        EXIT_FAILURE_HANDLER();
    }
    ListNode *current = list->head;
    for (int i = 0; i < index; i++) {
        current = current->next;
    }
    return current->value;
}

void freeList(List *list) {
    ListNode *current = list->head;
    while (current) {
        ListNode *temp = current;
        current = current->next;
        free(temp->value);
        free(temp);
    }
    free(list);
}

bool listContains(const List *list, const char *value) {
    if (!list) return false;  // <-- safety guard
    ListNode *current = list->head;
    while (current) {
        if (strcasecmp(current->value, value) == 0) {
            return true;
        }
        current = current->next;
    }
    return false;
}

