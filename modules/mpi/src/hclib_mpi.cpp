#include "hclib_mpi-internal.h"
#include "hclib-locality-graph.h"

#include <iostream>

#ifdef HCLIB_INSTRUMENT
#include "hclib-instrument.h"

enum MPI_FUNC_LABELS {
    MPI_Send_lbl = 0,
    MPI_Recv_lbl,
    MPI_Isend_lbl,
    MPI_Irecv_lbl,
    MPI_Allreduce_lbl,
    MPI_Allreduce_future_lbl,
    MPI_Bcast_lbl,
    MPI_Barrier_lbl,
    MPI_Allgather_lbl,
    MPI_Reduce_lbl,
    MPI_Waitall_lbl,
    N_MPI_FUNCS
};

const char *MPI_FUNC_NAMES[N_MPI_FUNCS] = {
    "MPI_Send",
    "MPI_Recv",
    "MPI_Isend",
    "MPI_Irecv",
    "MPI_Allreduce",
    "MPI_Allreduce_future",
    "MPI_Bcast",
    "MPI_Barrier",
    "MPI_Allgather",
    "MPI_Reduce",
    "MPI_Waitall"
};

static int event_ids[N_MPI_FUNCS];

#define MPI_START_OP(funcname) \
    const unsigned _event_id = hclib_register_event(event_ids[funcname##_lbl], \
            START, -1)
#define MPI_END_OP(funcname) \
    hclib_register_event(event_ids[funcname##_lbl], END, _event_id)

#else
#define MPI_START_OP(funcname)
#define MPI_END_OP(funcname)
#endif

static int nic_locale_id;
static hclib::locale_t *nic = NULL;

HCLIB_MODULE_INITIALIZATION_FUNC(mpi_pre_initialize) {
    nic_locale_id = hclib_add_known_locale_type("Interconnect");

#ifdef HCLIB_INSTRUMENT
    int i;
    for (i = 0; i < N_MPI_FUNCS; i++) {
        event_ids[i] = register_event_type((char *)MPI_FUNC_NAMES[i]);
    }
#endif
}

typedef struct _pending_mpi_op {
    MPI_Request req;
    hclib::promise_t *prom;
    struct _pending_mpi_op *next;
} pending_mpi_op;

pending_mpi_op *pending = NULL;

HCLIB_MODULE_INITIALIZATION_FUNC(mpi_post_initialize) {
    int provided;
    CHECK_MPI(MPI_Init_thread(NULL, NULL, MPI_THREAD_FUNNELED, &provided));
    assert(provided == MPI_THREAD_FUNNELED);

    int n_nics;
    hclib::locale_t **nics = hclib::get_all_locales_of_type(nic_locale_id,
            &n_nics);
    HASSERT(n_nics == 1);
    HASSERT(nics);
    HASSERT(nic == NULL);
    nic = nics[0];

    hclib_locale_mark_special(nic, "COMM");
}

HCLIB_MODULE_INITIALIZATION_FUNC(mpi_finalize) {
    MPI_Finalize();
}

void hclib::MPI_Comm_rank(MPI_Comm comm, int *rank) {
    CHECK_MPI(::MPI_Comm_rank(comm, rank));
}

void hclib::MPI_Comm_size(MPI_Comm comm, int *size) {
    CHECK_MPI(::MPI_Comm_size(comm, size));
}

void hclib::MPI_Send(void *buf, int count, MPI_Datatype datatype, int dest,
        int tag, MPI_Comm comm) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            MPI_START_OP(MPI_Send);
            CHECK_MPI(::MPI_Send(buf, count, datatype, dest, tag, comm));
            MPI_END_OP(MPI_Send);
        }, nic);
    });
}

void hclib::MPI_Recv(void *buf, int count, MPI_Datatype datatype, int source,
        int tag, MPI_Comm comm, MPI_Status *status) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            MPI_START_OP(MPI_Recv);
            CHECK_MPI(::MPI_Recv(buf, count, datatype, source, tag, comm,
                    status));
            MPI_END_OP(MPI_Recv);
        }, nic);
    });
}

static void poll_on_pending() {
    do {
        int pending_list_non_empty = 1;

        pending_mpi_op *prev = NULL;
        pending_mpi_op *op = pending;

        assert(op != NULL);

        while (op) {
            pending_mpi_op *next = op->next;

            int complete;
            CHECK_MPI(::MPI_Test(&op->req, &complete, MPI_STATUS_IGNORE));

            if (complete) {
                // Remove from singly linked list
                if (prev == NULL) {
                    /*
                     * If previous is NULL, we *may* be looking at the front of
                     * the list. It is also possible that another thread in the
                     * meantime came along and added an entry to the front of
                     * this singly-linked wait list, in which case we need to
                     * ensure we update its next rather than updating the list
                     * head. We do this by first trying to automatically update
                     * the list head to be the next of wait_set, and if we fail
                     * then we know we have a new head whose next points to
                     * wait_set and which should be updated.
                     */
                    pending_mpi_op *old_head = __sync_val_compare_and_swap(
                            &pending, op, op->next);
                    if (old_head != op) {
                        // Failed, someone else added a different head
                        assert(old_head->next == op);
                        old_head->next = op->next;
                        prev = old_head;
                    } else {
                        /*
                         * Success, new head is now wait_set->next. We want this
                         * polling task to exit if we just set the head to NULL.
                         * It is the responsibility of future async_when calls
                         * to restart it upon discovering a null head.
                         */
                        pending_list_non_empty = (op->next != NULL);
                    }
                } else {
                    /*
                     * If previous is non-null, we just adjust its next link to
                     * jump over the current node.
                     */
                    assert(prev->next == op);
                    prev->next = op->next;
                }

                op->prom->put(NULL);
                free(op);
            } else {
                prev = op;
            }

            op = next;
        }

        if (pending_list_non_empty) {
            hclib::yield_at(nic);
        } else {
            // Empty list
            break;
        }
    } while (true);
}

static void append_to_pending(pending_mpi_op *op) {
    op->next = pending;

    pending_mpi_op *old_head;
    while (1) {
        old_head = __sync_val_compare_and_swap(&pending, op->next, op);
        if (old_head != op->next) {
            op->next = old_head;
        } else {
            break;
        }
    }

    if (old_head == NULL) {
        hclib::async_at([] {
            poll_on_pending();
        }, nic);
    }
}

void hclib::MPI_Waitall(int count, hclib::future_t *array_of_requests[]) {
    MPI_START_OP(MPI_Waitall);
    for (int i = 0; i < count; i++) {
        array_of_requests[i]->wait();
    }
    MPI_END_OP(MPI_Waitall);
}

hclib::future_t *hclib::MPI_Isend(void *buf, int count, MPI_Datatype datatype,
        int dest, int tag, MPI_Comm comm) {
    hclib::promise_t *prom = new hclib::promise_t();

    hclib::async_nb_at([=] {
        MPI_START_OP(MPI_Isend);

        MPI_Request req;
        CHECK_MPI(::MPI_Isend(buf, count, datatype, dest, tag, comm, &req));

        pending_mpi_op *op = (pending_mpi_op *)malloc(sizeof(pending_mpi_op));
        assert(op);
        op->req = req;
        op->prom = prom;
        append_to_pending(op);

        MPI_END_OP(MPI_Isend);
    }, nic);

    return prom->get_future();
}

hclib::future_t *hclib::MPI_Irecv(void *buf, int count, MPI_Datatype datatype,
        int source, int tag, MPI_Comm comm) {
    hclib::promise_t *prom = new hclib::promise_t();

    hclib::async_nb_at([=] {
        MPI_START_OP(MPI_Irecv);
        MPI_Request req;
        CHECK_MPI(::MPI_Irecv(buf, count, datatype, source, tag, comm, &req));

        pending_mpi_op *op = (pending_mpi_op *)malloc(sizeof(pending_mpi_op));
        assert(op);
        op->req = req;
        op->prom = prom;
        append_to_pending(op);

        MPI_END_OP(MPI_Irecv);
    }, nic);

    return prom->get_future();
}

void hclib::MPI_Comm_dup(MPI_Comm comm, MPI_Comm *newcomm) {
    CHECK_MPI(::MPI_Comm_dup(comm, newcomm));
}

void hclib::MPI_Comm_split(MPI_Comm comm, int color, int key, MPI_Comm *newcomm) {
    CHECK_MPI(::MPI_Comm_split(comm, color, key, newcomm));
}

void hclib::MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
        MPI_Datatype datatype, MPI_Op op, MPI_Comm comm) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            MPI_START_OP(MPI_Allreduce);
            CHECK_MPI(::MPI_Allreduce(sendbuf, recvbuf, count, datatype, op,
                    comm));
            MPI_END_OP(MPI_Allreduce);
        }, nic);
    });
}

hclib::future_t *hclib::MPI_Allreduce_future(const void *sendbuf, void *recvbuf,
        int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm) {
    return hclib::async_nb_future_at([=] {
        MPI_START_OP(MPI_Allreduce_future);
        CHECK_MPI(::MPI_Allreduce(sendbuf, recvbuf, count, datatype, op,
                comm));
        MPI_END_OP(MPI_Allreduce_future);
    }, nic);
}

void hclib::MPI_Allgather(const void *sendbuf, int sendcount,
        MPI_Datatype sendtype, void *recvbuf, int recvcount,
        MPI_Datatype recvtype, MPI_Comm comm) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            MPI_START_OP(MPI_Allgather);
            CHECK_MPI(::MPI_Allgather(sendbuf, sendcount, sendtype, recvbuf,
                    recvcount, recvtype, comm));
            MPI_END_OP(MPI_Allgather);
        }, nic);
    });
}

void hclib::MPI_Bcast(void *buffer, int count, MPI_Datatype datatype, int root, 
        MPI_Comm comm) {
    hclib::finish([&] {
            hclib::async_nb_at([&] {
                MPI_START_OP(MPI_Bcast);
                CHECK_MPI(::MPI_Bcast(buffer, count, datatype, root, comm));
                MPI_END_OP(MPI_Bcast);
            }, nic);
    });
}

void hclib::MPI_Barrier(MPI_Comm comm) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            MPI_START_OP(MPI_Barrier);
            CHECK_MPI(::MPI_Barrier(comm));
            MPI_END_OP(MPI_Barrier);
        }, nic);
    });
}

void hclib::MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
        MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            MPI_START_OP(MPI_Reduce);
            CHECK_MPI(::MPI_Reduce(sendbuf, recvbuf, count, datatype, op, root,
                    comm));
            MPI_END_OP(MPI_Reduce);
        }, nic);
    });
}

HCLIB_REGISTER_MODULE("mpi", mpi_pre_initialize, mpi_post_initialize, mpi_finalize)
