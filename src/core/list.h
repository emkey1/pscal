//
//  list.h
//  pscal
//
//  Created by Michael Miller on 3/8/25.
//
#ifndef LIST_H
#define LIST_H
#include <stdbool.h>

typedef struct ListNode {
    char *value;
    struct ListNode *next;
} ListNode;

typedef struct {
    ListNode *head;
    ListNode *tail;
    int size;
} List;

List *createList(void);
void listAppend(List *list, const char *value);
int listSize(const List *list);
char *listGet(const List *list, int index);
void freeList(List *list);
bool listContains(const List *list, const char *value);

#endif // LIST_H

