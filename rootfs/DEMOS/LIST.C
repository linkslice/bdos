/* LIST.C - structs, pointers and malloc: a sorted linked list */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct node {
	char name[16];
	int age;
	struct node *next;
} Node;

Node *insert(Node *head, const char *name, int age)
{
	Node *n = malloc(sizeof(Node));
	Node **pp = &head;

	strcpy(n->name, name);
	n->age = age;
	while (*pp && (*pp)->age < age)
		pp = &(*pp)->next;
	n->next = *pp;
	*pp = n;
	return head;
}

int main(void)
{
	Node *list = NULL, *p;
	int total = 0, count = 0;

	list = insert(list, "Ada", 36);
	list = insert(list, "Grace", 85);
	list = insert(list, "Alan", 41);
	list = insert(list, "Dennis", 70);
	list = insert(list, "Sophie", 29);
	for (p = list; p; p = p->next) {
		printf("%-8s %3d\n", p->name, p->age);
		total += p->age;
		count++;
	}
	printf("average age %.1f\n", (double)total / count);
	while (list) {
		p = list->next;
		free(list);
		list = p;
	}
	return 0;
}
