#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>

#include "heap.h"

/* Returns true if random float in [0,1] is less than *
 * numerator/denominator.  Uses only integer math.    */
# define rand_under(numerator, denominator) \
  (rand() < ((RAND_MAX / denominator) * numerator))

/* Returns random integer in [min, max]. */
# define rand_range(min, max) ((rand() % (((max) + 1) - (min))) + (min))

typedef struct corridor_path {
  heap_node_t *hn;
  uint8_t pos[2];
  uint8_t from[2];
  int32_t cost;
} corridor_path_t;

typedef enum dim {
  dim_x,
  dim_y,
  num_dims
} dim_t;

typedef int16_t pair_t[num_dims];

#define DUNGEON_X              160
#define DUNGEON_Y              105
#define MIN_ROOMS              25
#define MAX_ROOMS              40
#define ROOM_MIN_X             7
#define ROOM_MIN_Y             5
#define ROOM_MAX_X             20
#define ROOM_MAX_Y             15
#define SAVE_DIR               ".rlg327"
#define DUNGEON_SAVE_FILE      "dungeon"
#define DUNGEON_SAVE_SEMANTIC  "RLG327-S2017"
#define DUNGEON_SAVE_VERSION   0U
  
#define mappair(pair) (d->map[pair[dim_y]][pair[dim_x]])
#define mapxy(x, y) (d->map[y][x])
#define hardnesspair(pair) (d->hardness[pair[dim_y]][pair[dim_x]])
#define hardnessxy(x, y) (d->hardness[y][x])

typedef enum __attribute__ ((__packed__)) terrain_type {
  ter_debug,
  ter_wall,
  ter_wall_immutable,
  ter_floor,
  ter_floor_room,
  ter_floor_hall,
} terrain_type_t;

typedef struct room {
  pair_t position;
  pair_t size;
} room_t;

typedef struct monster{
  heap_node_t *hn;
  uint8_t pos[2];
  uint8_t next_pos[2];
  char type;
  uint8_t speed;
}monster_t;

typedef struct tunnel {
  heap_node_t *hn;
  uint8_t pos[2];
  uint8_t from[2];
  int32_t cost;
} tunnel_t;

typedef struct non_tunnel {
  heap_node_t *hn;
  uint8_t pos[2];
  uint8_t from[2];
  int32_t cost;
} non_tunnel_t;

typedef struct dungeon {
  uint32_t num_rooms;
  room_t *rooms;
  terrain_type_t map[DUNGEON_Y][DUNGEON_X];
  /* Since hardness is usually not used, it would be expensive to pull it *
   * into cache every time we need a map cell, so we store it in a        *
   * parallel array, rather than using a structure to represent the       *
   * cells.  We may want a cell structure later, but from a performanace  *
   * perspective, it would be a bad idea to ever have the map be part of  *
   * that structure.  Pathfinding will require efficient use of the map,  *
   * and pulling in unnecessary data with each map cell would add a lot   *
   * of overhead to the memory system.                                    */
  uint8_t hardness[DUNGEON_Y][DUNGEON_X];
  tunnel_t tunneling[DUNGEON_Y][DUNGEON_X];
  non_tunnel_t non_tunneling[DUNGEON_Y][DUNGEON_X];  
  monster_t monster[DUNGEON_Y][DUNGEON_X];
} dungeon_t;

static uint32_t in_room(dungeon_t *d, int16_t y, int16_t x)
{
  int i;

  for (i = 0; i < d->num_rooms; i++) {
    if ((x >= d->rooms[i].position[dim_x]) &&
        (x < (d->rooms[i].position[dim_x] + d->rooms[i].size[dim_x])) &&
        (y >= d->rooms[i].position[dim_y]) &&
        (y < (d->rooms[i].position[dim_y] + d->rooms[i].size[dim_y]))) {
      return 1;
    }
  }

  return 0;
}

static int32_t corridor_path_cmp(const void *key, const void *with) {
  return ((corridor_path_t *) key)->cost - ((corridor_path_t *) with)->cost;
}

static void dijkstra_corridor(dungeon_t *d, pair_t from, pair_t to)
{
  static corridor_path_t path[DUNGEON_Y][DUNGEON_X], *p;
  static uint32_t initialized = 0;
  heap_t h;
  uint32_t x, y;

  if (!initialized) {
    for (y = 0; y < DUNGEON_Y; y++) {
      for (x = 0; x < DUNGEON_X; x++) {
        path[y][x].pos[dim_y] = y;
        path[y][x].pos[dim_x] = x;
      }
    }
    initialized = 1;
  }
  
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      path[y][x].cost = INT_MAX;
    }
  }

  path[from[dim_y]][from[dim_x]].cost = 0;

  heap_init(&h, corridor_path_cmp, NULL);

  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      if (mapxy(x, y) != ter_wall_immutable) {
        path[y][x].hn = heap_insert(&h, &path[y][x]);
      } else {
        path[y][x].hn = NULL;
      }
    }
  }

  while ((p = heap_remove_min(&h))) {
    p->hn = NULL;

    if ((p->pos[dim_y] == to[dim_y]) && p->pos[dim_x] == to[dim_x]) {
      for (x = to[dim_x], y = to[dim_y];
           (x != from[dim_x]) || (y != from[dim_y]);
           p = &path[y][x], x = p->from[dim_x], y = p->from[dim_y]) {
        if (mapxy(x, y) != ter_floor_room) {
          mapxy(x, y) = ter_floor_hall;
          hardnessxy(x, y) = 0;
        }
      }
      heap_delete(&h);
      return;
    }

    if ((path[p->pos[dim_y] - 1][p->pos[dim_x]    ].hn) &&
        (path[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost >
         p->cost + hardnesspair(p->pos))) {
      path[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost =
        p->cost + hardnesspair(p->pos);
      path[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y] - 1]
                                           [p->pos[dim_x]    ].hn);
    }
    if ((path[p->pos[dim_y]    ][p->pos[dim_x] - 1].hn) &&
        (path[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost >
         p->cost + hardnesspair(p->pos))) {
      path[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost =
        p->cost + hardnesspair(p->pos);
      path[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y]    ]
                                           [p->pos[dim_x] - 1].hn);
    }
    if ((path[p->pos[dim_y]    ][p->pos[dim_x] + 1].hn) &&
        (path[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost >
         p->cost + hardnesspair(p->pos))) {
      path[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost =
        p->cost + hardnesspair(p->pos);
      path[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y]    ]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((path[p->pos[dim_y] + 1][p->pos[dim_x]    ].hn) &&
        (path[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost >
         p->cost + hardnesspair(p->pos))) {
      path[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost =
        p->cost + hardnesspair(p->pos);
      path[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y] + 1]
                                           [p->pos[dim_x]    ].hn);
    }
  }
}

/* This is a cut-and-paste of the above.  The code is modified to  *
 * calculate paths based on inverse hardnesses so that we get a    *
 * high probability of creating at least one cycle in the dungeon. */
static void dijkstra_corridor_inv(dungeon_t *d, pair_t from, pair_t to)
{
  static corridor_path_t path[DUNGEON_Y][DUNGEON_X], *p;
  static uint32_t initialized = 0;
  heap_t h;
  uint32_t x, y;

  if (!initialized) {
    for (y = 0; y < DUNGEON_Y; y++) {
      for (x = 0; x < DUNGEON_X; x++) {
        path[y][x].pos[dim_y] = y;
        path[y][x].pos[dim_x] = x;
      }
    }
    initialized = 1;
  }
  
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      path[y][x].cost = INT_MAX;
    }
  }

  path[from[dim_y]][from[dim_x]].cost = 0;

  heap_init(&h, corridor_path_cmp, NULL);

  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      if (mapxy(x, y) != ter_wall_immutable) {
        path[y][x].hn = heap_insert(&h, &path[y][x]);
      } else {
        path[y][x].hn = NULL;
      }
    }
  }

  while ((p = heap_remove_min(&h))) {
    p->hn = NULL;

    if ((p->pos[dim_y] == to[dim_y]) && p->pos[dim_x] == to[dim_x]) {
      for (x = to[dim_x], y = to[dim_y];
           (x != from[dim_x]) || (y != from[dim_y]);
           p = &path[y][x], x = p->from[dim_x], y = p->from[dim_y]) {
        if (mapxy(x, y) != ter_floor_room) {
          mapxy(x, y) = ter_floor_hall;
          hardnessxy(x, y) = 0;
        }
      }
      heap_delete(&h);
      return;
    }

#define hardnesspair_inv(p) (in_room(d, p[dim_y], p[dim_x]) ? \
                             224                            : \
                             (255 - hardnesspair(p)))

    if ((path[p->pos[dim_y] - 1][p->pos[dim_x]    ].hn) &&
        (path[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost >
         p->cost + hardnesspair_inv(p->pos))) {
      path[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost =
        p->cost + hardnesspair_inv(p->pos);
      path[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y] - 1]
                                           [p->pos[dim_x]    ].hn);
    }
    if ((path[p->pos[dim_y]    ][p->pos[dim_x] - 1].hn) &&
        (path[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost >
         p->cost + hardnesspair_inv(p->pos))) {
      path[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost =
        p->cost + hardnesspair_inv(p->pos);
      path[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y]    ]
                                           [p->pos[dim_x] - 1].hn);
    }
    if ((path[p->pos[dim_y]    ][p->pos[dim_x] + 1].hn) &&
        (path[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost >
         p->cost + hardnesspair_inv(p->pos))) {
      path[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost =
        p->cost + hardnesspair_inv(p->pos);
      path[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y]    ]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((path[p->pos[dim_y] + 1][p->pos[dim_x]    ].hn) &&
        (path[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost >
         p->cost + hardnesspair_inv(p->pos))) {
      path[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost =
        p->cost + hardnesspair_inv(p->pos);
      path[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      path[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, path[p->pos[dim_y] + 1]
                                           [p->pos[dim_x]    ].hn);
    }
  }
}

/* Chooses a random point inside each room and connects them with a *
 * corridor.  Random internal points prevent corridors from exiting *
 * rooms in predictable locations.                                  */
static int connect_two_rooms(dungeon_t *d, room_t *r1, room_t *r2)
{
  pair_t e1, e2;

  e1[dim_y] = rand_range(r1->position[dim_y],
                         r1->position[dim_y] + r1->size[dim_y] - 1);
  e1[dim_x] = rand_range(r1->position[dim_x],
                         r1->position[dim_x] + r1->size[dim_x] - 1);
  e2[dim_y] = rand_range(r2->position[dim_y],
                         r2->position[dim_y] + r2->size[dim_y] - 1);
  e2[dim_x] = rand_range(r2->position[dim_x],
                         r2->position[dim_x] + r2->size[dim_x] - 1);

  /*  return connect_two_points_recursive(d, e1, e2);*/
  dijkstra_corridor(d, e1, e2);

  return 0;
}

static int create_cycle(dungeon_t *d)
{
  /* Find the (approximately) farthest two rooms, then connect *
   * them by the shortest path using inverted hardnesses.      */

  int32_t max, tmp, i, j, p, q;
  pair_t e1, e2;

  for (i = max = 0; i < d->num_rooms - 1; i++) {
    for (j = i + 1; j < d->num_rooms; j++) {
      tmp = (((d->rooms[i].position[dim_x] - d->rooms[j].position[dim_x])  *
              (d->rooms[i].position[dim_x] - d->rooms[j].position[dim_x])) +
             ((d->rooms[i].position[dim_y] - d->rooms[j].position[dim_y])  *
              (d->rooms[i].position[dim_y] - d->rooms[j].position[dim_y])));
      if (tmp > max) {
        max = tmp;
        p = i;
        q = j;
      }
    }
  }

  /* Can't simply call connect_two_rooms() because it doesn't *
   * use inverse hardnesses, so duplicate it here.            */
  e1[dim_y] = rand_range(d->rooms[p].position[dim_y],
                         (d->rooms[p].position[dim_y] +
                          d->rooms[p].size[dim_y] - 1));
  e1[dim_x] = rand_range(d->rooms[p].position[dim_x],
                         (d->rooms[p].position[dim_x] +
                          d->rooms[p].size[dim_x] - 1));
  e2[dim_y] = rand_range(d->rooms[q].position[dim_y],
                         (d->rooms[q].position[dim_y] +
                          d->rooms[q].size[dim_y] - 1));
  e2[dim_x] = rand_range(d->rooms[q].position[dim_x],
                         (d->rooms[q].position[dim_x] +
                          d->rooms[q].size[dim_x] - 1));

  dijkstra_corridor_inv(d, e1, e2);

  return 0;
}

static int connect_rooms(dungeon_t *d)
{
  uint32_t i;

  for (i = 1; i < d->num_rooms; i++) {
    connect_two_rooms(d, d->rooms + i - 1, d->rooms + i);
  }

  create_cycle(d);

  return 0;
}

int gaussian[5][5] = {
  {  1,  4,  7,  4,  1 },
  {  4, 16, 26, 16,  4 },
  {  7, 26, 41, 26,  7 },
  {  4, 16, 26, 16,  4 },
  {  1,  4,  7,  4,  1 }
};

typedef struct queue_node {
  int x, y;
  struct queue_node *next;
} queue_node_t;

static int smooth_hardness(dungeon_t *d)
{
  int32_t i, x, y;
  int32_t s, t, p, q;
  queue_node_t *head, *tail, *tmp;
  uint8_t hardness[DUNGEON_Y][DUNGEON_X];
#ifdef DUMP_HARDNESS_IMAGES
  FILE *out;
#endif
  memset(&hardness, 0, sizeof (hardness));

  /* Seed with some values */
  for (i = 1; i < 255; i += 5) {
    do {
      x = rand() % DUNGEON_X;
      y = rand() % DUNGEON_Y;
    } while (hardness[y][x]);
    hardness[y][x] = i;
    if (i == 1) {
      head = tail = malloc(sizeof (*tail));
    } else {
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
    }
    tail->next = NULL;
    tail->x = x;
    tail->y = y;
  }

#ifdef DUMP_HARDNESS_IMAGES
  out = fopen("seeded.pgm", "w");
  fprintf(out, "P5\n%u %u\n255\n", DUNGEON_X, DUNGEON_Y);
  fwrite(&hardness, sizeof (hardness), 1, out);
  fclose(out);
#endif

  /* Diffuse the vaules to fill the space */
  while (head) {
    x = head->x;
    y = head->y;
    i = hardness[y][x];

    if (x - 1 >= 0 && y - 1 >= 0 && !hardness[y - 1][x - 1]) {
      hardness[y - 1][x - 1] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x - 1;
      tail->y = y - 1;
    }
    if (x - 1 >= 0 && !hardness[y][x - 1]) {
      hardness[y][x - 1] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x - 1;
      tail->y = y;
    }
    if (x - 1 >= 0 && y + 1 < DUNGEON_Y && !hardness[y + 1][x - 1]) {
      hardness[y + 1][x - 1] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x - 1;
      tail->y = y + 1;
    }
    if (y - 1 >= 0 && !hardness[y - 1][x]) {
      hardness[y - 1][x] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x;
      tail->y = y - 1;
    }
    if (y + 1 < DUNGEON_Y && !hardness[y + 1][x]) {
      hardness[y + 1][x] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x;
      tail->y = y + 1;
    }
    if (x + 1 < DUNGEON_X && y - 1 >= 0 && !hardness[y - 1][x + 1]) {
      hardness[y - 1][x + 1] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x + 1;
      tail->y = y - 1;
    }
    if (x + 1 < DUNGEON_X && !hardness[y][x + 1]) {
      hardness[y][x + 1] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x + 1;
      tail->y = y;
    }
    if (x + 1 < DUNGEON_X && y + 1 < DUNGEON_Y && !hardness[y + 1][x + 1]) {
      hardness[y + 1][x + 1] = i;
      tail->next = malloc(sizeof (*tail));
      tail = tail->next;
      tail->next = NULL;
      tail->x = x + 1;
      tail->y = y + 1;
    }

    tmp = head;
    head = head->next;
    free(tmp);
  }

  /* And smooth it a bit with a gaussian convolution */
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      for (s = t = p = 0; p < 5; p++) {
        for (q = 0; q < 5; q++) {
          if (y + (p - 2) >= 0 && y + (p - 2) < DUNGEON_Y &&
              x + (q - 2) >= 0 && x + (q - 2) < DUNGEON_X) {
            s += gaussian[p][q];
            t += hardness[y + (p - 2)][x + (q - 2)] * gaussian[p][q];
          }
        }
      }
      d->hardness[y][x] = t / s;
    }
  }
  /* Let's do it again, until it's smooth like Kenny G. */
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      for (s = t = p = 0; p < 5; p++) {
        for (q = 0; q < 5; q++) {
          if (y + (p - 2) >= 0 && y + (p - 2) < DUNGEON_Y &&
              x + (q - 2) >= 0 && x + (q - 2) < DUNGEON_X) {
            s += gaussian[p][q];
            t += hardness[y + (p - 2)][x + (q - 2)] * gaussian[p][q];
          }
        }
      }
      d->hardness[y][x] = t / s;
    }
  }

#ifdef DUMP_HARDNESS_IMAGES
  out = fopen("diffused.pgm", "w");
  fprintf(out, "P5\n%u %u\n255\n", DUNGEON_X, DUNGEON_Y);
  fwrite(&hardness, sizeof (hardness), 1, out);
  fclose(out);

  out = fopen("smoothed.pgm", "w");
  fprintf(out, "P5\n%u %u\n255\n", DUNGEON_X, DUNGEON_Y);
  fwrite(&d->hardness, sizeof (d->hardness), 1, out);
  fclose(out);
#endif

  return 0;
}

static int empty_dungeon(dungeon_t *d)
{
  uint8_t x, y;

  smooth_hardness(d);
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      mapxy(x, y) = ter_wall;
      d->monster[y][x].type = 'z';
      if (y == 0 || y == DUNGEON_Y - 1 ||
          x == 0 || x == DUNGEON_X - 1) {
        mapxy(x, y) = ter_wall_immutable;
        hardnessxy(x, y) = 255;
      }
    }
  }

  return 0;
}

static int place_rooms(dungeon_t *d)
{
  pair_t p;
  uint32_t i;
  int success;
  room_t *r;
  uint8_t hardness[DUNGEON_Y][DUNGEON_X];
  uint32_t x, y;
  struct timeval tv, start;

  /* Placing rooms is 2D bin packing.  Bin packing (2D or otherwise) is NP-  *
   * Complete, meaning (among other things) that there is no known algorithm *
   * to solve the problem in less than exponential time.  There are          *
   * hacks and approximation algorithms that can function more efficiently,  *
   * but we're going to forgoe those in favor of using a timeout.  If we     *
   * can't place all of our rooms in 1 second (and some change), we'll abort *
   * this attempt and start over.                                            */
  gettimeofday(&start, NULL);

  memcpy(&hardness, &d->hardness, sizeof (hardness));

  for (success = 0; !success; ) {
    success = 1;
    for (i = 0; success && i < d->num_rooms; i++) {
      r = d->rooms + i;
      r->position[dim_x] = 1 + rand() % (DUNGEON_X - 2 - r->size[dim_x]);
      r->position[dim_y] = 1 + rand() % (DUNGEON_Y - 2 - r->size[dim_y]);
      for (p[dim_y] = r->position[dim_y] - 1;
           success && p[dim_y] < r->position[dim_y] + r->size[dim_y] + 1;
           p[dim_y]++) {
        for (p[dim_x] = r->position[dim_x] - 1;
             success && p[dim_x] < r->position[dim_x] + r->size[dim_x] + 1;
             p[dim_x]++) {
          if (mappair(p) >= ter_floor) {
            gettimeofday(&tv, NULL);
            if ((tv.tv_sec - start.tv_sec) > 1) {
              memcpy(&d->hardness, &hardness, sizeof (hardness));
              return 1;
            }
            success = 0;
            /* empty_dungeon() regenerates the hardness map, which   *
             * is prohibitively expensive to do in a loop like this, *
             * so instead, we'll use a copy.                         */
            memcpy(&d->hardness, &hardness, sizeof (hardness));
            for (y = 1; y < DUNGEON_Y - 1; y++) {
              for (x = 1; x < DUNGEON_X - 1; x++) {
                mapxy(x, y) = ter_wall;
              }
            }
          } else if ((p[dim_y] != r->position[dim_y] - 1)              &&
                     (p[dim_y] != r->position[dim_y] + r->size[dim_y]) &&
                     (p[dim_x] != r->position[dim_x] - 1)              &&
                     (p[dim_x] != r->position[dim_x] + r->size[dim_x])) {
            mappair(p) = ter_floor_room;
            hardnesspair(p) = 0;
          }
        }
      }
    }
  }

  return 0;
}

static int make_rooms(dungeon_t *d)
{
  uint32_t i;

  for (i = MIN_ROOMS; i < MAX_ROOMS && rand_under(6, 8); i++)
    ;
  d->num_rooms = i;
  if (d->rooms) {
    /* If we're restarting due to a timeout in place_rooms(), we need to     *
     * free our rooms array or we'll lose the pointer (could use realloc()). */
    free(d->rooms);
  }
  d->rooms = malloc(sizeof (*d->rooms) * d->num_rooms);

  for (i = 0; i < d->num_rooms; i++) {
    d->rooms[i].size[dim_x] = ROOM_MIN_X;
    d->rooms[i].size[dim_y] = ROOM_MIN_Y;
    while (rand_under(3, 4) && d->rooms[i].size[dim_x] < ROOM_MAX_X) {
      d->rooms[i].size[dim_x]++;
    }
    while (rand_under(3, 4) && d->rooms[i].size[dim_y] < ROOM_MAX_Y) {
      d->rooms[i].size[dim_y]++;
    }
  }

  return 0;
}

int gen_dungeon(dungeon_t *d)
{
  do {
    make_rooms(d);
  } while (place_rooms(d));
  connect_rooms(d);

  return 0;
}

void render_dungeon(dungeon_t *d)
{
  pair_t p;

  for (p[dim_y] = 0; p[dim_y] < DUNGEON_Y; p[dim_y]++) {
    for (p[dim_x] = 0; p[dim_x] < DUNGEON_X; p[dim_x]++) {
      switch (mappair(p)) {
      case ter_wall:
      case ter_wall_immutable:
	if(d->monster[p[dim_y]][p[dim_x]].type == 'z')
	  putchar(' ');
	else
	  putchar(d->monster[p[dim_y]][p[dim_x]].type);
        break;
      case ter_floor:
      case ter_floor_room:
	if(d->monster[p[dim_y]][p[dim_x]].type == 'z')
	  putchar('.');
	else
	  putchar(d->monster[p[dim_y]][p[dim_x]].type);
        break;
      case ter_floor_hall:
	if(d->monster[p[dim_y]][p[dim_x]].type == 'z')
	  putchar('#');
	else
	  putchar(d->monster[p[dim_y]][p[dim_x]].type);
        break;
      case ter_debug:
        putchar('*');
        fprintf(stderr, "Debug character at %d, %d\n", p[dim_y], p[dim_x]);
        break;
      }
    }
    putchar('\n');
  }
}

void delete_dungeon(dungeon_t *d)
{
  free(d->rooms);
}

void init_dungeon(dungeon_t *d)
{
  d->rooms = NULL;
  empty_dungeon(d);
}

int write_dungeon_map(dungeon_t *d, FILE *f)
{
  uint32_t x, y;

  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      fwrite(&d->hardness[y][x], sizeof (unsigned char), 1, f);
    }
  }

  return 0;
}

int write_rooms(dungeon_t *d, FILE *f)
{
  uint32_t i;
  uint8_t p;

  for (i = 0; i < d->num_rooms; i++) {
    /* write order is xpos, ypos, width, height */
    p = d->rooms[i].position[dim_x];
    fwrite(&p, 1, 1, f);
    p = d->rooms[i].position[dim_y];
    fwrite(&p, 1, 1, f);
    p = d->rooms[i].size[dim_x];
    fwrite(&p, 1, 1, f);
    p = d->rooms[i].size[dim_y];
    fwrite(&p, 1, 1, f);
  }

  return 0;
}

uint32_t calculate_dungeon_size(dungeon_t *d)
{
  return (20 /* The semantic, version, and size */     +
          (DUNGEON_X * DUNGEON_Y) /* The hardnesses */ +
          (d->num_rooms * 4) /* Four bytes per room */);
}

int makedirectory(char *dir)
{
  char *slash;

  for (slash = dir + strlen(dir); slash > dir && *slash != '/'; slash--)
    ;

  if (slash == dir) {
    return 0;
  }

  if (mkdir(dir, 0700)) {
    if (errno != ENOENT && errno != EEXIST) {
      fprintf(stderr, "mkdir(%s): %s\n", dir, strerror(errno));
      return 1;
    }
    if (*slash != '/') {
      return 1;
    }
    *slash = '\0';
    if (makedirectory(dir)) {
      *slash = '/';
      return 1;
    }

    *slash = '/';
    if (mkdir(dir, 0700) && errno != EEXIST) {
      fprintf(stderr, "mkdir(%s): %s\n", dir, strerror(errno));
      return 1;
    }
  }

  return 0;
}

int write_dungeon(dungeon_t *d, char *file)
{
  char *home;
  char *filename;
  FILE *f;
  size_t len;
  uint32_t be32;

  if (!file) {
    if (!(home = getenv("HOME"))) {
      fprintf(stderr, "\"HOME\" is undefined.  Using working directory.\n");
      home = ".";
    }

    len = (strlen(home) + strlen(SAVE_DIR) + strlen(DUNGEON_SAVE_FILE) +
           1 /* The NULL terminator */                                 +
           2 /* The slashes */);

    filename = malloc(len * sizeof (*filename));
    sprintf(filename, "%s/%s/", home, SAVE_DIR);
    makedirectory(filename);
    strcat(filename, DUNGEON_SAVE_FILE);

    if (!(f = fopen(filename, "w"))) {
      perror(filename);
      free(filename);

      return 1;
    }
    free(filename);
  } else {
    if (!(f = fopen(file, "w"))) {
      perror(file);
      exit(-1);
    }
  }

  /* The semantic, which is 12 bytes, 0-11 */
  fwrite(DUNGEON_SAVE_SEMANTIC, 1, strlen(DUNGEON_SAVE_SEMANTIC), f);

  /* The version, 4 bytes, 12-15 */
  be32 = htobe32(DUNGEON_SAVE_VERSION);
  fwrite(&be32, sizeof (be32), 1, f);

  /* The size of the file, 4 bytes, 16-19 */
  be32 = htobe32(calculate_dungeon_size(d));
  fwrite(&be32, sizeof (be32), 1, f);

  /* The dungeon map, 16800 bytes, 20-16819 */
  write_dungeon_map(d, f);

  /* And the rooms, num_rooms * 4 bytes, 16820-end */
  write_rooms(d, f);

  fclose(f);

  return 0;
}

int read_dungeon_map(dungeon_t *d, FILE *f)
{
  uint32_t x, y;

  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      fread(&d->hardness[y][x], sizeof (d->hardness[y][x]), 1, f);
      if (d->hardness[y][x] == 0) {
        /* Mark it as a corridor.  We can't recognize room cells until *
         * after we've read the room array, which we haven't done yet. */
        d->map[y][x] = ter_floor_hall;
      } else if (d->hardness[y][x] == 255) {
        d->map[y][x] = ter_wall_immutable;
      } else {
        d->map[y][x] = ter_wall;
      }
    }
  }


  return 0;
}

int read_rooms(dungeon_t *d, FILE *f)
{
  uint32_t i;
  uint32_t x, y;
  uint8_t p;

  for (i = 0; i < d->num_rooms; i++) {
    fread(&p, 1, 1, f);
    d->rooms[i].position[dim_x] = p;
    fread(&p, 1, 1, f);
    d->rooms[i].position[dim_y] = p;
    fread(&p, 1, 1, f);
    d->rooms[i].size[dim_x] = p;
    fread(&p, 1, 1, f);
    d->rooms[i].size[dim_y] = p;
    /* After reading each room, we need to reconstruct them in the dungeon. */
    for (y = d->rooms[i].position[dim_y];
         y < d->rooms[i].position[dim_y] + d->rooms[i].size[dim_y];
         y++) {
      for (x = d->rooms[i].position[dim_x];
           x < d->rooms[i].position[dim_x] + d->rooms[i].size[dim_x];
           x++) {
        mapxy(x, y) = ter_floor_room;
      }
    }
  }

  return 0;
}

int calculate_num_rooms(uint32_t dungeon_bytes)
{
  return ((dungeon_bytes -
          (20 /* The semantic, version, and size */       +
           (DUNGEON_X * DUNGEON_Y) /* The hardnesses */)) /
          4 /* Four bytes per room */);
}

int read_dungeon(dungeon_t *d, char *file)
{
  char semantic[13];
  uint32_t be32;
  FILE *f;
  char *home;
  size_t len;
  char *filename;
  struct stat buf;

  if (!file) {
    if (!(home = getenv("HOME"))) {
      fprintf(stderr, "\"HOME\" is undefined.  Using working directory.\n");
      home = ".";
    }

    len = (strlen(home) + strlen(SAVE_DIR) + strlen(DUNGEON_SAVE_FILE) +
           1 /* The NULL terminator */                                 +
           2 /* The slashes */);

    filename = malloc(len * sizeof (*filename));
    sprintf(filename, "%s/%s/%s", home, SAVE_DIR, DUNGEON_SAVE_FILE);

    if (!(f = fopen(filename, "r"))) {
      perror(filename);
      free(filename);
      exit(-1);
    }

    if (stat(filename, &buf)) {
      perror(filename);
      exit(-1);
    }

    free(filename);
  } else {
    if (!(f = fopen(file, "r"))) {
      perror(file);
      exit(-1);
    }
    if (stat(file, &buf)) {
      perror(file);
      exit(-1);
    }
  }

  d->num_rooms = 0;

  fread(semantic, sizeof(semantic) - 1, 1, f);
  semantic[12] = '\0';
  if (strncmp(semantic, DUNGEON_SAVE_SEMANTIC, 12)) {
    fprintf(stderr, "Not an RLG327 save file.\n");
    exit(-1);
  }
  fread(&be32, sizeof (be32), 1, f);
  if (be32toh(be32) != 0) { /* Since we expect zero, be32toh() is a no-op. */
    fprintf(stderr, "File version mismatch.\n");
    exit(-1);
  }
  fread(&be32, sizeof (be32), 1, f);
  if (buf.st_size != be32toh(be32)) {
    fprintf(stderr, "File size mismatch.\n");
    exit(-1);
  }
  read_dungeon_map(d, f);
  d->num_rooms = calculate_num_rooms(buf.st_size);
  d->rooms = malloc(sizeof (*d->rooms) * d->num_rooms);
  read_rooms(d, f);

  fclose(f);

  return 0;
}

int read_pgm(dungeon_t *d, char *pgm)
{
  FILE *f;
  char s[80];
  uint8_t gm[103][158];
  uint32_t x, y;
  uint32_t i;

  if (!(f = fopen(pgm, "r"))) {
    perror(pgm);
    exit(-1);
  }

  if (!fgets(s, 80, f) || strncmp(s, "P5", 2)) {
    fprintf(stderr, "Expected P5\n");
    exit(-1);
  }
  if (!fgets(s, 80, f) || s[0] != '#') {
    fprintf(stderr, "Expected comment\n");
    exit(-1);
  }
  if (!fgets(s, 80, f) || strncmp(s, "158 103", 5)) {
    fprintf(stderr, "Expected 158 103\n");
    exit(-1);
  }
  if (!fgets(s, 80, f) || strncmp(s, "255", 2)) {
    fprintf(stderr, "Expected 255\n");
    exit(-1);
  }

  fread(gm, 1, 158 * 103, f);

  fclose(f);

  /* In our gray map, treat black (0) as corridor, white (255) as room, *
   * all other values as a hardness.  For simplicity, treat every white *
   * cell as its own room, so we have to count white after reading the  *
   * image in order to allocate the room array.                         */
  for (d->num_rooms = 0, y = 0; y < 103; y++) {
    for (x = 0; x < 158; x++) {
      if (!gm[y][x]) {
        d->num_rooms++;
      }
    }
  }
  d->rooms = malloc(sizeof (*d->rooms) * d->num_rooms);

  for (i = 0, y = 0; y < 103; y++) {
    for (x = 0; x < 158; x++) {
      if (!gm[y][x]) {
        d->rooms[i].position[dim_x] = x + 1;
        d->rooms[i].position[dim_y] = y + 1;
        d->rooms[i].size[dim_x] = 1;
        d->rooms[i].size[dim_y] = 1;
        i++;
        d->map[y + 1][x + 1] = ter_floor_room;
        d->hardness[y + 1][x + 1] = 0;
      } else if (gm[y][x] == 255) {
        d->map[y + 1][x + 1] = ter_floor_hall;
        d->hardness[y + 1][x + 1] = 0;
      } else {
        d->map[y + 1][x + 1] = ter_wall;
        d->hardness[y + 1][x + 1] = gm[y][x];
      }
    }
  }

  for (x = 0; x < 160; x++) {
    d->map[0][x] = ter_wall_immutable;
    d->hardness[0][x] = 255;
    d->map[104][x] = ter_wall_immutable;
    d->hardness[104][x] = 255;
  }
  for (y = 1; y < 104; y++) {
    d->map[y][0] = ter_wall_immutable;
    d->hardness[y][0] = 255;
    d->map[y][159] = ter_wall_immutable;
    d->hardness[y][159] = 255;
  }

  return 0;
}

void usage(char *name)
{
  fprintf(stderr,
          "Usage: %s [-r|--rand <seed>] [-l|--load [<file>]]\n"
          "          [-s|--save [<file>]] [-i|--image <pgm file>]\n",
          name);

  exit(-1);
}

//Places pc in room
void place_pc(dungeon_t* d, int *playerY, int *playerX){
  int placed = 0;
  int y, x;
  while(!placed){
    y = rand() % 105;
    x = rand() % 160;
    if(d->map[y][x] == ter_floor_room && d->monster[y][x].type == 'z'){
      d->monster[y][x].type = '@';
      d->monster[y][x].speed = 10;
      d->monster[y][x].pos[0] = y;
      d->monster[y][x].pos[1] = x;
      placed = 1;
    }
  }
  *playerY = y;
  *playerX = x;
}

static int32_t non_tunnel_cmp(const void *key, const void *with) {
  return ((non_tunnel_t *) key)->cost - ((non_tunnel_t *) with)->cost;
}

//Calculates distance from PC for non-tunneling monsters
void non_tunneling(dungeon_t *dungeon, int playerY, int playerX){
  static non_tunnel_t *p;
  static uint32_t initialized = 0;
  heap_t heap;
  int y, x;
  
  if (!initialized) {
    for (y = 0; y < DUNGEON_Y; y++) {
      for (x = 0; x < DUNGEON_X; x++) {
        dungeon->non_tunneling[y][x].pos[dim_y] = y;
        dungeon->non_tunneling[y][x].pos[dim_x] = x;
      }
    }
    initialized = 1;
  }
  
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      dungeon->non_tunneling[y][x].cost = INT_MAX;
    }
  }

  dungeon->non_tunneling[playerY][playerX].cost = 0;

  heap_init(&heap, non_tunnel_cmp, NULL);

  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      if (dungeon->hardness[y][x] == 0) {
        dungeon->non_tunneling[y][x].hn = heap_insert(&heap, &dungeon->non_tunneling[y][x]);
      } else {
        dungeon->non_tunneling[y][x].hn = NULL;
      }
    }
  }

  while ((p = heap_remove_min(&heap))) {
    p->hn = NULL;

    if ((dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y] - 1]
                                           [p->pos[dim_x] - 1].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y] - 1]
                                           [p->pos[dim_x]    ].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y] - 1]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y]    ]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y] + 1]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y] + 1]
                                           [p->pos[dim_x]    ].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y] + 1]
                                           [p->pos[dim_x] - 1].hn);
    }
    if ((dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].hn) &&
        (dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost >
         p->cost + 1)) {
      dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost =
        p->cost + 1;
      dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      dungeon->non_tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&heap, dungeon->non_tunneling[p->pos[dim_y]    ]
                                           [p->pos[dim_x] - 1].hn);
    }
  }  
}

static int32_t tunnel_cmp(const void *key, const void *with) {
  return ((non_tunnel_t *) key)->cost - ((non_tunnel_t *) with)->cost;
}

//Calculates distance from PC for tunneling monsters
void tunneling(dungeon_t *dungeon, int playerY, int playerX){
  static tunnel_t *p;
  static uint32_t initialized = 0;
  heap_t h;
  uint32_t x, y;

  if (!initialized) {
    for (y = 0; y < DUNGEON_Y; y++) {
      for (x = 0; x < DUNGEON_X; x++) {
        dungeon->tunneling[y][x].pos[dim_y] = y;
        dungeon->tunneling[y][x].pos[dim_x] = x;
      }
    }
    initialized = 1;
  }
  
  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      dungeon->tunneling[y][x].cost = INT_MAX;
    }
  }

  dungeon->tunneling[playerY][playerX].cost = 0;

  heap_init(&h, tunnel_cmp, NULL);

  for (y = 0; y < DUNGEON_Y; y++) {
    for (x = 0; x < DUNGEON_X; x++) {
      if (dungeon->map[y][x] != ter_wall_immutable) {
        dungeon->tunneling[y][x].hn = heap_insert(&h, &dungeon->tunneling[y][x]);
      } else {
        dungeon->tunneling[y][x].hn = NULL;
      }
      }
  }

  while ((p = heap_remove_min(&h))) {
    p->hn = NULL;
    
    int cost = 0;
    if(dungeon->hardness[y][x] >= 0 && dungeon->hardness[y][x] <= 84){
      cost = 1;
    }
    else if(dungeon->hardness[y][x] >= 85 && dungeon->hardness[y][x] <= 170){
      cost = 2;
    }
    else
      cost = 3;

    if ((dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].hn) &&
        (dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y] - 1]
                                           [p->pos[dim_x] - 1].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].hn) &&
        (dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y] - 1]
                                           [p->pos[dim_x]    ].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].hn) &&
        (dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y] - 1][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y] - 1]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].hn) &&
        (dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y]    ]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].hn) &&
        (dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] + 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y] + 1]
                                           [p->pos[dim_x] + 1].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].hn) &&
        (dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x]    ].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y] + 1]
                                           [p->pos[dim_x]    ].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].hn) &&
        (dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y] + 1][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y] + 1]
                                           [p->pos[dim_x] - 1].hn);
    }
    if ((dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].hn) &&
        (dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost >
         p->cost + cost)) {
      dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].cost =
        p->cost + cost;
      dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_y] = p->pos[dim_y];
      dungeon->tunneling[p->pos[dim_y]    ][p->pos[dim_x] - 1].from[dim_x] = p->pos[dim_x];
      heap_decrease_key_no_replace(&h, dungeon->tunneling[p->pos[dim_y]    ]
                                           [p->pos[dim_x] - 1].hn);
    }
  }
}

void generate_monsters(dungeon_t *dungeon, int numMonsters){
  int i;
  for(i = 0; i < numMonsters; i++){
    //50/50 chance of each attribute
    int intelligence = rand() % 2;
    int telepathy = rand() % 2;
    int tunneling = rand() % 2;
    int erratic = rand() % 2;
    int placed = 0;
    int x, y;
    //Finds a place in a room where there isn't a character for new monster
    while(!placed){
      y = rand() % 105;
      x = rand() % 160;
      if(dungeon->map[y][x] == ter_floor_room && dungeon->monster[y][x].type == 'z'){
	placed = 1;
      }
    }
    //Based on attributes adds monster to the dungeon
    if(intelligence == 0 && telepathy == 0 && tunneling == 0 && erratic == 0){
      dungeon->monster[y][x].type = '0';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 0 && tunneling == 0 && erratic == 1){
      dungeon->monster[y][x].type = '1';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 0 && tunneling == 1 && erratic == 0){
      dungeon->monster[y][x].type = '2';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 0 && tunneling == 1 && erratic == 1){
      dungeon->monster[y][x].type = '3';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 1 && tunneling == 0 && erratic == 0){
      dungeon->monster[y][x].type = '4';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 1 && tunneling == 0 && erratic == 1){
      dungeon->monster[y][x].type = '5';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 1 && tunneling == 1 && erratic == 0){
      dungeon->monster[y][x].type = '6';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 0 && telepathy == 1 && tunneling == 1 && erratic == 1){
      dungeon->monster[y][x].type = '7';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 0 && tunneling == 0 && erratic == 0){
      dungeon->monster[y][x].type = '8';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 0 && tunneling == 0 && erratic == 1){
      dungeon->monster[y][x].type = '9';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 0 && tunneling == 1 && erratic == 0){
      dungeon->monster[y][x].type = 'a';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 0 && tunneling == 1 && erratic == 1){
      dungeon->monster[y][x].type = 'b';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 1 && tunneling == 0 && erratic == 0){
      dungeon->monster[y][x].type = 'c';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 1 && tunneling == 0 && erratic == 1){
      dungeon->monster[y][x].type = 'd';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else if(intelligence == 1 && telepathy == 1 && tunneling == 1 && erratic == 0){
      dungeon->monster[y][x].type = 'e';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    else{
      dungeon->monster[y][x].type = 'f';
      dungeon->monster[y][x].speed = rand() % 16 + 5;
    }
    dungeon->monster[y][x].pos[0] = y;
    dungeon->monster[y][x].pos[1] = x;
  }
}

void moveStraight(monster_t *monster, int playerY, int playerX){
      if(monster->pos[0] == playerY && monster->pos[1] < playerX){
	monster->next_pos[0] = monster->pos[0];
	monster->next_pos[1] = monster->pos[1] + 1;
      }
      else if(monster->pos[0] == playerY && monster->pos[1] > playerX){
	monster->next_pos[0] = monster->pos[0];
	monster->next_pos[1] = monster->pos[1] - 1;
      }
      else if(monster->pos[0] < playerY && monster->pos[1] == playerX){
	monster->next_pos[0] = monster->pos[0] + 1;
	monster->next_pos[1] = monster->pos[1];
      }
      else if(monster->pos[0] > playerY && monster->pos[1] == playerX){
	monster->next_pos[0] = monster->pos[0] - 1;
	monster->next_pos[1] = monster->pos[1];
      }
      else if(monster->pos[0] < playerY && monster->pos[1] < playerX){
	monster->next_pos[0] = monster->pos[0] + 1;
	monster->next_pos[1] = monster->pos[1] + 1;
      }
      else if(monster->pos[0] < playerY && monster->pos[1] > playerX){
	monster->next_pos[0] = monster->pos[0] + 1;
	monster->next_pos[1] = monster->pos[1] - 1;
      }
      else if(monster->pos[0] > playerY && monster->pos[1] < playerX){
	monster->next_pos[0] = monster->pos[0] - 1;
	monster->next_pos[1] = monster->pos[1] + 1;
      }
      else{
	monster->next_pos[0] = monster->pos[0] - 1;
	monster->next_pos[1] = monster->pos[1] - 1;
      }
}

void findShortest(dungeon_t *dungeon, monster_t *monster, int playerY, int playerX, int tunnel){
  int ny = 0, nx = 0, best = INT_MAX;
  //Finds closest move for tunneling monsters
  if(tunnel){
    if(dungeon->tunneling[monster->pos[0] - 1][monster->pos[1] - 1].cost < best){
      ny = -1;
      nx = -1;
    }
    else if(dungeon->tunneling[monster->pos[0] - 1][monster->pos[1]    ].cost < best){
      ny = -1;
      nx = 0;
    }
    else if(dungeon->tunneling[monster->pos[0] - 1][monster->pos[1] + 1].cost < best){
      ny = -1;
      nx = 1;
    }
    else if(dungeon->tunneling[monster->pos[0]    ][monster->pos[1] + 1].cost < best){
      ny = 0;
      nx = 1;
    }
    else if(dungeon->tunneling[monster->pos[0] + 1][monster->pos[1] + 1].cost < best){
      ny = 1;
      nx = 1;
    }
    else if(dungeon->tunneling[monster->pos[0] + 1][monster->pos[1]    ].cost < best){
      ny = 1;
      nx = 0;
    }
    else if(dungeon->tunneling[monster->pos[0] + 1][monster->pos[1] - 1].cost < best){
      ny = 1;
      nx = -1;
    }
    else{
      ny = 0;
      nx = -1;
    }
  }
  //Finds closest move for non-tunneling monsters
  else{
    if(dungeon->non_tunneling[monster->pos[0] - 1][monster->pos[1] - 1].cost < best){
      ny = -1;
      nx = -1;
    }
    else if(dungeon->non_tunneling[monster->pos[0] - 1][monster->pos[1]    ].cost < best){
      ny = -1;
      nx = 0;
    }
    else if(dungeon->non_tunneling[monster->pos[0] - 1][monster->pos[1] + 1].cost < best){
      ny = -1;
      nx = 1;
    }
    else if(dungeon->non_tunneling[monster->pos[0]    ][monster->pos[1] + 1].cost < best){
      ny = 0;
      nx = 1;
    }
    else if(dungeon->non_tunneling[monster->pos[0] + 1][monster->pos[1] + 1].cost < best){
      ny = 1;
      nx = 1;
    }
    else if(dungeon->non_tunneling[monster->pos[0] + 1][monster->pos[1]    ].cost < best){
      ny = 1;
      nx = 0;
    }
    else if(dungeon->non_tunneling[monster->pos[0] + 1][monster->pos[1] - 1].cost < best){
      ny = 1;
      nx = -1;
    }
    else{
      ny = 0;
      nx = -1;
    }
  }
  monster->next_pos[0] = monster->pos[0] + ny;
  monster->next_pos[1] = monster->pos[1] + nx;
}

int generate_move(dungeon_t *dungeon, heap_t *heap, monster_t *monster, int playerY, int playerX){
  int erratic = rand() % 2;
  int sameRoom = 0;
  int i;
  //Checks if monster is in same room as PC
  for(i = 0; i < dungeon->num_rooms; i++){
    if(dungeon->rooms[i].position[dim_y] < monster->pos[0] && dungeon->rooms[i].position[dim_y] + dungeon->rooms[i].size[dim_y] > monster->pos[0] &&
       dungeon->rooms[i].position[dim_x] < monster->pos[1] && dungeon->rooms[i].position[dim_x] + dungeon->rooms[i].size[dim_x] > monster->pos[1] &&
       dungeon->rooms[i].position[dim_y] < playerY && dungeon->rooms[i].position[dim_y] + dungeon->rooms[i].size[dim_y] > playerY &&
       dungeon->rooms[i].position[dim_y] < playerX && dungeon->rooms[i].position[dim_y] + dungeon->rooms[i].size[dim_y] > playerX){
      sameRoom = 1;
    }
  }

  if(monster->type == '@'){
    //PC moves randomly
    int ny = rand_range(-1, 1);
    int nx = rand_range(-1, 1);
    monster->next_pos[0] = monster->pos[0] + ny;
    monster->next_pos[1] = monster->pos[1] + nx;
  }
  else if(monster->type == '0'){
    //Monster moves only if PC is in its line of sight
    if(sameRoom){
      moveStraight(monster, playerY, playerX);
    }
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == '1'){
    //Monster moves either erratically or else if PC is in line of sight
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else if(sameRoom){
      moveStraight(monster, playerY, playerX);
    }
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == '2'){
    //Monster can tunnel and moves if PC is in line of sight (doesn't ever need to tunnel)
    if(sameRoom){
      moveStraight(monster, playerY, playerX);
    }
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == '3'){
    //Monster can tunnel and either moves erratically or if PC is in line of sight
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else if(sameRoom){
      moveStraight(monster, playerY, playerX);
    }
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == '4'){
    //Monster knows where the PC is and moves towards it in straight line
    moveStraight(monster, playerY, playerX);
  }
  else if(monster->type == '5'){
    //Monster either moves errtically or knows where the PC is and moves towards it in straight line
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else
      moveStraight(monster, playerY, playerX);
  }
  else if(monster->type == '6'){
    //Monster moves towards PC in straight line and can tunnel
    moveStraight(monster, playerY, playerX);
  }
  else if(monster->type =='7'){
    //Monster can tunnel and either moves erratically or in a straight line towards PC
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else
      moveStraight(monster, playerY, playerX);
  }
  else if(monster->type == '8'){
    //Monster moves along shortest path towards the PC when in line of sight or if last known position is known
    if(sameRoom)
      findShortest(dungeon, monster, playerY, playerX, 0);
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == '9'){
    //Monster either moves eratically or along shortest path when PC is in line of sight or last position is known
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else if(sameRoom)
      findShortest(dungeon, monster, playerY, playerX, 0);
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == 'a'){
    //Monster can tunnel and moves along shortest path when PC is in line of sight or is last position is known
    if(sameRoom)
      findShortest(dungeon, monster, playerY, playerX, 1);
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
    }
  }
  else if(monster->type == 'b'){
    //Monster can tunnel and either moves erratically of moves along shortest path when PC is in line of sight or is last position is known
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
            printf("AFSW");
    }
    else if(sameRoom){
      findShortest(dungeon, monster, playerY, playerX, 1);
            printf("AFSW");
    }
    else{
      monster->next_pos[0] = monster->pos[0];
      monster->next_pos[1] = monster->pos[1];
      printf("AFSW");
    }
  }
  else if(monster->type == 'c'){
    //Monster moves along shortest path towards current position of PC
    findShortest(dungeon, monster, playerY, playerX, 0);
  }
  else if(monster->type == 'd'){
    //Monster either moves erratically or along shortest path towards current position of PC
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else
      findShortest(dungeon, monster, playerY, playerX, 0);
  }
  else if(monster->type == 'e'){
    //Monster can tunnel and moves along shortest path towards current position of PC
    findShortest(dungeon, monster, playerY, playerX, 1);
  }
  else{
    //Monster can tunnel and either moves erratically or moves along shortest path towards current position of PC
    if(erratic){
      int ny = rand_range(-1, 1);
      int nx = rand_range(-1, 1);
      monster->next_pos[0] = monster->pos[0] + ny;
      monster->next_pos[1] = monster->pos[1] + nx;
    }
    else
      findShortest(dungeon, monster, playerY, playerX, 1);
  }
  
  
  if((monster->type == '@' || monster->type == '0' || monster->type == '1' || monster->type == '4' || monster->type =='5' || monster->type == '8' || monster->type == '9' || monster->type == 'c' || monster->type == 'd') &&
     dungeon->hardness[monster->next_pos[0]][monster->next_pos[1]] != 0)
    return 0;
  else{
    printf("type: %c speed: %d cur y: %d cur x: %d next y: %d next x: %d\n", monster->type, monster->speed, monster->pos[0], monster->pos[1], monster->next_pos[0], monster->next_pos[1]);
    monster->hn = heap_insert(heap, monster);
    return 1;
  }
  
}

static int32_t move_cmp(const void *key, const void *with) {
  return ((monster_t *) key)->speed/1000 - ((monster_t *) with)->speed/1000;
}

int main(int argc, char *argv[])
{
  dungeon_t d;
  time_t seed;
  struct timeval tv;
  uint32_t i;
  uint32_t do_load, do_save, do_seed, do_image;
  uint32_t long_arg;
  char *save_file;
  char *load_file;
  char *pgm_file;
  uint32_t numMonsters;

  /* Default behavior: Seed with the time, generate a new dungeon, *
   * and don't write to disk.                                      */
  do_load = do_save = do_image = 0;
  do_seed = 1;
  save_file = load_file = NULL;

  /* The project spec requires '--load' and '--save'.  It's common  *
   * to have short and long forms of most switches (assuming you    *
   * don't run out of letters).  For now, we've got plenty.  Long   *
   * forms use whole words and take two dashes.  Short forms use an *
    * abbreviation after a single dash.  We'll add '--rand' (to     *
   * specify a random seed), which will take an argument of it's    *
   * own, and we'll add short forms for all three commands, '-l',   *
   * '-s', and '-r', respectively.  We're also going to allow an    *
   * optional argument to load to allow us to load non-default save *
   * files.  No means to save to non-default locations, however.    *
   * And the final switch, '--image', allows me to create a dungeon *
   * from a PGM image, so that I was able to create those more      *
   * interesting test dungeons for you.                             */
 
 if (argc > 1) {
    for (i = 1, long_arg = 0; i < argc; i++, long_arg = 0) {
      if (argv[i][0] == '-') { /* All switches start with a dash */
        if (argv[i][1] == '-') {
          argv[i]++;    /* Make the argument have a single dash so we can */
          long_arg = 1; /* handle long and short args at the same place.  */
        }
        switch (argv[i][1]) {
        case 'r':
          if ((!long_arg && argv[i][2]) ||
              (long_arg && strcmp(argv[i], "-rand")) ||
              argc < ++i + 1 /* No more arguments */ ||
              !sscanf(argv[i], "%lu", &seed) /* Argument is not an integer */) {
            usage(argv[0]);
          }
          do_seed = 0;
          break;
        case 'l':
          if ((!long_arg && argv[i][2]) ||
              (long_arg && strcmp(argv[i], "-load"))) {
            usage(argv[0]);
          }
          do_load = 1;
          if ((argc > i + 1) && argv[i + 1][0] != '-') {
            /* There is another argument, and it's not a switch, so *
             * we'll treat it as a save file and try to load it.    */
            load_file = argv[++i];
          }
          break;
        case 's':
          if ((!long_arg && argv[i][2]) ||
              (long_arg && strcmp(argv[i], "-save"))) {
            usage(argv[0]);
          }
          do_save = 1;
          if ((argc > i + 1) && argv[i + 1][0] != '-') {
            /* There is another argument, and it's not a switch, so *
             * we'll treat it as a save file and try to load it.    */
            save_file = argv[++i];
          }
          break;
        case 'i':
          if ((!long_arg && argv[i][2]) ||
              (long_arg && strcmp(argv[i], "-image"))) {
            usage(argv[0]);
          }
          do_image = 1;
          if ((argc > i + 1) && argv[i + 1][0] != '-') {
            /* There is another argument, and it's not a switch, so *
             * we'll treat it as a save file and try to load it.    */
            pgm_file = argv[++i];
          }
          break;
	case 'n':
	  numMonsters = atoi(argv[++i]);
	  break;
        default:
          usage(argv[0]);
        }
      } else { /* No dash */
        usage(argv[0]);
      }
    }
  }

  if (do_seed) {
    /* Allows me to generate more than one dungeon *
     * per second, as opposed to time().           */
    gettimeofday(&tv, NULL);
    seed = (tv.tv_usec ^ (tv.tv_sec << 20)) & 0xffffffff;
  }

  printf("Seed is %ld.\n", seed);
  srand(seed);

  init_dungeon(&d);

  if (do_load) {
    read_dungeon(&d, load_file);
  } else if (do_image) {
    read_pgm(&d, pgm_file);
  } else {
    gen_dungeon(&d);
  }

  int playerY, playerX;
  place_pc(&d, &playerY, &playerX);

  //If monsters were put into the game
  if(numMonsters > 0){
    generate_monsters(&d, numMonsters);

    non_tunneling(&d, playerY, playerX);
  
    tunneling(&d, playerY, playerX);

    render_dungeon(&d);

    heap_t heap;
    heap_init(&heap, move_cmp, NULL);
  
    int j, k;
    for(j = 0; j < 105; j++){
      for(k = 0; k < 160; k++){
	if(d.monster[j][k].type != 'z')
	  while(!generate_move(&d, &heap, &d.monster[j][k], playerY, playerX));
	else
	  d.monster[j][k].hn = NULL;
      }
    }

    monster_t *move;
      
    int playing = 1;
    while(playing && (move = heap_remove_min(&heap))){

      if(d.monster[move->next_pos[0]][move->next_pos[1]].type != 'z' && d.monster[move->next_pos[0]][move->next_pos[1]].type != d.monster[move->pos[0]][move->pos[1]].type){
	//If collision with player game ends
	if(move->next_pos[0] == playerY && move->next_pos[1] == playerX){
	  printf("\n\n\n\n\n\n\n\nGame Over\n");
	  playing = 0;
	  break;
	}
	//Else consumes monster in that space
	else{
	
	}
      }
      //If next space is rock
      else if(d.hardness[move->next_pos[0]][move->next_pos[1]] != 0){
	d.hardness[move->next_pos[0]][move->next_pos[1]] -= 82;
	if(d.hardness[move->next_pos[0]][move->next_pos[1]] <= 0){
	  //Makes space a hallway
	  d.hardness[move->next_pos[0]][move->next_pos[1]] = 0;
	  d.map[move->next_pos[0]][move->next_pos[1]] = ter_floor_hall;
	  //Moves monster into space
	  d.monster[move->next_pos[0]][move->next_pos[1]] = *move;
	  d.monster[move->next_pos[0]][move->next_pos[1]].pos[0] = move->next_pos[0];
	  d.monster[move->next_pos[0]][move->next_pos[1]].pos[1] = move->next_pos[1];
	  d.monster[move->next_pos[0]][move->next_pos[1]].hn = NULL;
	  d.monster[move->pos[0]][move->pos[1]].type = 'z';
	}
      }
      //If monster moves to next space with no collision and no tunneling
      else{
	  d.monster[move->next_pos[0]][move->next_pos[1]] = *move;
	  d.monster[move->next_pos[0]][move->next_pos[1]].pos[0] = move->next_pos[0];
	  d.monster[move->next_pos[0]][move->next_pos[1]].pos[1] = move->next_pos[1];
	  d.monster[move->next_pos[0]][move->next_pos[1]].hn = NULL;
	//If the monster moves out of its current space reset the space to have no monster
	if(move->pos[0] != move->next_pos[0] || move->pos[1] != move->next_pos[1])
	  d.monster[move->pos[0]][move->pos[1]].type = 'z';
      }
    
      //If player moves, recalculate distances and re-render dungeon
      if(d.monster[move->next_pos[0]][move->next_pos[1]].type == '@'){
	playerY = move->next_pos[0];
	playerX = move->next_pos[1];
	
	non_tunneling(&d, playerY, playerX);
  
	tunneling(&d, playerY, playerX);

	render_dungeon(&d);
      }
    
      //heap_decrease_key_no_replace(&heap, move->hn);
      printf("Generating for current: %c old pos: %c\n", d.monster[move->next_pos[0]][move->next_pos[1]].type, d.monster[move->pos[0]][move->pos[1]].type);
      while(!generate_move(&d, &heap, &d.monster[move->next_pos[0]][move->next_pos[1]], playerY, playerX));
      usleep(100000);
    }
  }
  else{
      non_tunneling(&d, playerY, playerX);
  
      tunneling(&d, playerY, playerX);

      render_dungeon(&d);
  }
  
  if (do_save) {
    write_dungeon(&d, save_file);
  }

  delete_dungeon(&d);

  return 0;
}