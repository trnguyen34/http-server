#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include "rwlock.h"

#include <stdio.h>

typedef struct HashTable hashTable;
typedef struct Node node;

struct Node {
    char *uri;
    rwlock_t *rwlock;
    node *next;
};

struct HashTable {
    node **list;
    size_t size;
};

hashTable *create_ht(size_t size);
node *create_node(char *key);
size_t hash_function(hashTable *ht, char *uri);
node *insert(hashTable *ht, char *uri);
node *search(hashTable *ht, char *uri);
void delete_ht(hashTable *ht);

#endif /* HASH_TABLE_H */
