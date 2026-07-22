# Design

Pretty graphs

Graph State Machine

## Error cases

## **TODO** common stuff

### Process Graph

```{.dot}
digraph {
  subgraph cluster {
    label="CLONE_FILES";
    keeper [label="Keeper"];
    listener [label="Listener"];
    keeper -> listener [label="clone(2)"];
  }

  worker1 [label="Worker"];
  worker2 [label="..."]
  listener1 -> worker1 [label="fork(2)\lexec(3)"];
  listener2 -> worker2 [label="fork(2)\lexec(3)"];

  listener1 -> listener1 [label="1. accept(2)",style=dashed];
  listener1 -> worker1 [label="2. incoming\nconnection",style=dashed];

  worker2 -> listener2 [label="1. connect\nto...",style=dashed];
  listener2 -> listener2 [label="2. connect(2)",style=dashed];
  listener2 -> worker2 [label="3. outgoing\nconnection",style=dashed];
}
```

### Shared Memory

::: {.figure}
```c
struct shared_memory {
    atomic_size_t size;
    struct connection connections[];
};

struct connection {
    atomic_int state;
    struct connection_endpoint downstream;
    struct connection_endpoint upstream;
};
```

Shared memory layout
::::

### Shared File Descriptors
