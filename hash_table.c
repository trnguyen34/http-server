#include "hash_table.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

hashTable *create_ht(size_t size) {
    hashTable *ht = (hashTable *) malloc(sizeof(hashTable));
    if (ht == NULL)
        return NULL;

    ht->list = (node **) malloc(size * sizeof(node *));
    if (ht->list == NULL)
        return NULL;

    ht->size = size;

    for (size_t i = 0; i < size; i++)
        ht->list[i] = NULL;

    return ht;
}

node *create_node(char *uri) {
    node *new_node = (node *) malloc(sizeof(node));
    if (new_node == NULL)
        return NULL;

    new_node->rwlock = rwlock_new(N_WAY, 1);
    new_node->uri = strdup(uri);
    new_node->next = NULL;

    return new_node;
}

size_t hash_function(hashTable *ht, char *uri) {
    size_t bucket_index;
    size_t sum = 0, factor = 31;

    for (size_t i = 0; i < strlen(uri); i++) {
        sum = ((sum % ht->size) + (((int) uri[i]) * factor) % ht->size) % ht->size;
        factor = ((factor % __INT16_MAX__) * (31 % __INT16_MAX__)) % __INT16_MAX__;
    }

    bucket_index = sum;
    return bucket_index;
}

node *insert(hashTable *ht, char *uri) {
    node *new_node = create_node(uri);
    if (new_node == NULL) {
        return NULL;
    }

    size_t bucket_index = hash_function(ht, uri);

    if (ht->list[bucket_index] == NULL) {
        ht->list[bucket_index] = new_node;
    } else {
        new_node->next = ht->list[bucket_index];
        ht->list[bucket_index] = new_node;
    }

    return new_node;
}

node *search(hashTable *ht, char *uri) {
    int bucket_index = hash_function(ht, uri);
    node *current = ht->list[bucket_index];

    while (current != NULL) {
        if (strcmp(current->uri, uri) == 0) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

void delete_ht(hashTable *ht) {
    for (size_t i = 0; i < ht->size; i++) {
        node *current = ht->list[i];
        while (current != NULL) {
            node *temp = current;
            current = current->next;
            rwlock_delete(&temp->rwlock);
            temp->rwlock = NULL;
            free(temp->uri);
            temp->uri = NULL;
            free(temp->rwlock);
            temp->rwlock = NULL;
            free(temp);
            temp = NULL;
        }
    }

    free(ht->list);
    ht->list = NULL;
    free(ht);
    ht = NULL;
    return;
}
