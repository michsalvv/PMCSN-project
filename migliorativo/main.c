#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "./DES/rngs.h"
#include "./DES/rvgs.h"
#include "./config.h"
#include "./utils.h"
#include "math.h"

// Function Prototypes
// --------------------------------------------------------
double getArrival(double current);
double getService(enum block_types type, int stream);
void process_arrival();
void process_completion(compl completion);
void init_blocks();
void activate_servers();
void deactivate_servers();
void update_network();
void init_config();
void enqueue_balancing(server *s, struct job *j);
void finite_horizon_simulation(int stop_time, int repetitions);
void finite_horizon_run(int stop, int repetition);
void infinite_horizon_simulation(int stop);
void infinite_horizon_batch(int stop, int repetition, int k);
void end_servers();
void clear_environment();
void write_rt_csv_finite();
void write_rt_csv_infinite();
void init_config();
void print_results_finite();
void print_results_infinite();
void init_network(int rep);
void set_time_slot(int rep);
void reset_statistics();
// ---------------------------------------------------------

network_configuration config;
struct clock_t clock;                          // Mantiene le informazioni sul clock di simulazione
struct block blocks[NUM_BLOCKS];               // Mantiene lo stato dei singoli blocchi della rete
sorted_completions global_sorted_completions;  // Tiene in una lista ordinata tutti i completamenti nella rete cosi da ottenere il prossimo in O(log(N))
network_status global_network_status;          // Tiene lo stato complessivo della rete
static const sorted_completions empty_sorted;
static const network_status empty_network;

double arrival_rate;
double lambdas[] = {LAMBDA_1, LAMBDA_2, LAMBDA_3};
int completed;
int dropped;
int bypassed;
bool slot_switched[3];

int stop_simulation = TIME_SLOT_1 + TIME_SLOT_2 + TIME_SLOT_3;
int streamID;  // Stream da selezionare per generare il tempo di servizio
int num_slot;
char *simulation_mode;
FILE *finite_csv;

double repetitions_costs[NUM_REPETITIONS];
double response_times[] = {0, 0, 0};
double statistics[NUM_REPETITIONS][3];
double infinite_statistics[200000];
double global_means_p[BATCH_K][NUM_BLOCKS];
double global_means_p_fin[NUM_REPETITIONS][3][NUM_BLOCKS];
double global_loss[BATCH_K];
// ------------------------------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: ./simulate-migliorativo <FINITE/INFINITE/TEST> <TIME_SLOT>\n");
        exit(0);
    }
    simulation_mode = argv[1];
    num_slot = atoi(argv[2]);

    if (str_compare(simulation_mode, "FINITE") == 0) {
        PlantSeeds(521312312);
        finite_horizon_simulation(stop_simulation, NUM_REPETITIONS);

    } else if (str_compare(simulation_mode, "INFINITE") == 0) {
        PlantSeeds(231232132);
        infinite_horizon_simulation(num_slot);
    } else {
        printf("Specify mode FINITE/INFINITE or TEST\n");
        exit(0);
    }
}

// Stampa a schermo delle percentuali di perdita del blocco 4
void print_ploss() {
    double loss_perc = (float)blocks[GREEN_PASS].total_bypassed / (float)blocks[GREEN_PASS].total_arrivals;
    printf("P_LOSS: %f\n", loss_perc);
}

// Esegue le ripetizioni di singole run a orizzonte finito
void finite_horizon_simulation(int stop_time, int repetitions) {
    printf("\n\n==== Finite Horizon Simulation | sim_time %d | #repetitions #%d ====", stop_simulation, NUM_REPETITIONS);
    init_config();
    print_configuration(&config);

    char filename[100];
    snprintf(filename, 100, "results/finite/continuos_finite.csv");

    finite_csv = open_csv(filename);

    for (int r = 0; r < repetitions; r++) {
        finite_horizon_run(stop_time, r);
        if (r == 0 && strcmp(simulation_mode, "FINITE") == 0) {
            print_p_on_csv(&global_network_status, clock.current, 2);
        }
        clear_environment();

        print_percentage(r, repetitions, r - 1);
    }

    fclose(finite_csv);
    write_rt_csv_finite();
    print_results_finite();
}

// Esegue una simulazione ad orizzonte infinito tramite il metodo delle batch means
void infinite_horizon_simulation(int slot) {
    printf("\n\n==== Infinite Horizon Simulation for slot %d | #batch %d====", slot, BATCH_K);
    init_config();
    print_configuration(&config);
    arrival_rate = lambdas[slot];
    int b = BATCH_B;
    clear_environment();
    init_network(0);
    global_network_status.time_slot = slot;
    update_network();
    for (int k = 0; k < BATCH_K; k++) {
        infinite_horizon_batch(slot, b, k);
        print_percentage(k, BATCH_K, k - 1);
    }
    write_rt_csv_infinite(slot);
    end_servers();
    print_results_infinite(slot);
}

// Esegue una singola run di simulazione ad orizzonte finito
void finite_horizon_run(int stop_time, int repetition) {
    init_network(0);
    int n = 1;

    while (clock.arrival <= stop_time) {
        set_time_slot(repetition);
        compl *nextCompletion = &global_sorted_completions.sorted_list[0];
        server *nextCompletionServer = nextCompletion->server;

        clock.next = min(nextCompletion->value, clock.arrival);
        for (int i = 0; i < NUM_BLOCKS; i++) {
            if (i == GREEN_PASS) {
                blocks[i].area.node += (clock.next - clock.current) * blocks[GREEN_PASS].jobInBlock;
            }
            for (int j = 0; j < MAX_SERVERS; j++) {  // Non posso fare il ciclo su num_online_servers altrimenti non aggiorno le statistiche di quelli con need_resched
                server *s = &global_network_status.server_list[i][j];

                if (s->jobInTotal > 0 && s->used) {
                    s->area.node += (clock.next - clock.current) * s->jobInTotal;
                    s->area.queue += (clock.next - clock.current) * s->jobInQueue;
                    s->area.service += (clock.next - clock.current);
                }
            }
        }
        clock.current = clock.next;  // Avanzamento del clock al valore del prossimo evento

        if (clock.current == clock.arrival) {
            process_arrival();
        } else {
            process_completion(*nextCompletion);
        }
        if (clock.current >= (n - 1) * 300 && clock.current < (n)*300 && completed > 16 && clock.arrival < stop_time) {
            calculate_statistics_clock(&global_network_status, blocks, clock.current, finite_csv);
            n++;
        }
    }
    end_servers();
    repetitions_costs[repetition] = calculate_cost(&global_network_status);
    calculate_statistics_fin(&global_network_status, clock.current, response_times, global_means_p_fin, repetition);

    for (int i = 0; i < 3; i++) {
        statistics[repetition][i] = response_times[i];
    }
}

// Resetta l'ambiante di esecuzione tra due run ad orizzonte finito
void clear_environment() {
    global_sorted_completions = empty_sorted;
    global_network_status = empty_network;

    for (int block_type = 0; block_type < NUM_BLOCKS; block_type++) {
        for (int j = 0; j < MAX_SERVERS; j++) {
            if (global_network_status.server_list[block_type][j].used) {
                global_network_status.server_list[block_type][j].area.service = 0;
                global_network_status.server_list[block_type][j].area.node = 0;
                global_network_status.server_list[block_type][j].area.queue = 0;
            }
        }
    }
}

// Calcola il tempo online per i server al termine della simulazione
void end_servers() {
    for (int j = 0; j < NUM_BLOCKS; j++) {
        for (int i = 0; i < MAX_SERVERS; i++) {
            server *s = &global_network_status.server_list[j][i];
            if (s->online == ONLINE) {
                s->time_online += (clock.current - s->last_online);
                s->last_online = clock.current;
            }
        }
    }
}

// Esegue una simulazione ad orizzonte infinito di un singolo batch
void infinite_horizon_batch(int slot, int b, int k) {
    int n = 0;
    int q = 0;
    global_network_status.time_slot = slot;
    double old;

    while (true) {
        compl *nextCompletion = &global_sorted_completions.sorted_list[0];
        server *nextCompletionServer = nextCompletion->server;
        if (n >= b) {
            clock.next = nextCompletion->value;  // Ottengo il prossimo evento
            if (clock.next == INFINITY) {
                break;
            }
        } else {
            clock.next = min(nextCompletion->value, clock.arrival);
        }
        for (int i = 0; i < NUM_BLOCKS; i++) {
            if (i == GREEN_PASS) {
                blocks[i].area.node += (clock.next - clock.current) * blocks[GREEN_PASS].jobInBlock;
            }
            for (int j = 0; j < MAX_SERVERS; j++) {  // Non posso fare il ciclo su num_online_servers altrimenti non aggiorno le statistiche di quelli con need_resched
                server *s = &global_network_status.server_list[i][j];

                if (s->jobInTotal > 0 && s->used) {
                    s->area.node += (clock.next - clock.current) * s->jobInTotal;
                    s->area.queue += (clock.next - clock.current) * s->jobInQueue;
                    s->area.service += (clock.next - clock.current);
                }
            }
        }
        clock.current = clock.next;  // Avanzamento del clock al valore del prossimo evento

        if (clock.current == clock.arrival) {
            process_arrival();
            n++;
        } else {
            process_completion(*nextCompletion);
            q++;
        }
    }
    calculate_statistics_inf(&global_network_status, blocks, (clock.current - clock.batch_current), infinite_statistics, k);
    for (int i = 0; i < NUM_BLOCKS; i++) {
        double p = 0;
        int n = 0;
        for (int j = 0; j < MAX_SERVERS; j++) {
            server s = global_network_status.server_list[i][j];
            if (s.used == 1) {
                p += (s.sum.service / clock.current);
                n++;
            }
        }
        if (i == GREEN_PASS) {
            double loss_perc = (float)blocks[i].total_bypassed / (float)blocks[i].total_arrivals;
            global_loss[k] = loss_perc;
        }
        global_means_p[k][i] = p / n;
    }
    reset_statistics();
}

// Genera un tempo di arrivo secondo la distribuzione Esponenziale
double getArrival(double current) {
    double arrival = current;
    SelectStream(254);
    arrival += Exponential(1 / arrival_rate);
    return arrival;
}

// Genera un tempo di servizio esponenziale di media specificata e stream del servente individuato
double getService(enum block_types type, int stream) {
    SelectStream(stream);

    switch (type) {
        case TEMPERATURE_CTRL:
            return Exponential(SERV_TEMPERATURE_CTRL);
        case TICKET_BUY:
            return Exponential(SERV_TICKET_BUY);
        case TICKET_GATE:
            return Exponential(SERV_TICKET_GATE);
        case SEASON_GATE:
            return Exponential(SERV_SEASON_GATE);
        case GREEN_PASS:
            return Exponential(SERV_GREEN_PASS);
        default:
            return 0;
    }
}
// Inizializza tutti i serventi presenti nel sistema
void init_blocks() {
    for (int block_type = 0; block_type < NUM_BLOCKS; block_type++) {
        blocks[block_type].type = block_type;
        blocks[block_type].jobInBlock = 0;
        blocks[block_type].jobInQueue = 0;
        blocks[block_type].total_arrivals = 0;
        blocks[block_type].total_completions = 0;
        blocks[block_type].total_bypassed = 0;
        blocks[block_type].area.node = 0;
        blocks[block_type].area.service = 0;
        blocks[block_type].area.queue = 0;

        for (int i = 0; i < MAX_SERVERS; i++) {
            server s;
            s.id = i;
            s.status = IDLE;
            s.online = OFFLINE;
            s.used = NOTUSED;
            s.need_resched = false;
            s.block = &blocks[block_type];
            s.stream = streamID++;
            s.sum.served = 0;
            s.sum.service = 0.0;
            s.time_online = 0.0;
            s.last_online = 0.0;
            s.jobInQueue = 0;
            s.jobInTotal = 0;
            s.completions = 0;
            s.arrivals = 0;

            s.head_service = NULL;
            s.tail = NULL;
            s.area.node = 0;
            s.area.service = 0;
            s.area.queue = 0;

            global_network_status.server_list[block_type][i] = s;

            compl c = {&global_network_status.server_list[block_type][i], INFINITY};
            insertSorted(&global_sorted_completions, c);
        }
    }
}

// Disattiva un certo numero di server per il blocco, raggiungendo il numero specificato nella configurazione
void deactivate_servers(int block) {
    int start = 0;
    int slot = global_network_status.time_slot;
    start = global_network_status.num_online_servers[block];

    for (int i = start - 1; i >= config.slot_config[slot][block]; i--) {
        server *s = &global_network_status.server_list[block][i];

        if (s->status == BUSY) {
            s->need_resched = true;
        } else {
            s->online = OFFLINE;
            s->time_online += (clock.current - s->last_online);
            s->last_online = clock.current;
        }
        global_network_status.num_online_servers[block] = config.slot_config[slot][block];
    }
}

// Attiva un certo numero di server per il blocco, raggiungendo il numero specificato nella configurazione
void activate_servers(int block) {
    int start = 0;
    int slot = global_network_status.time_slot;

    start = global_network_status.num_online_servers[block];
    for (int i = start; i < config.slot_config[slot][block]; i++) {
        server *s = &global_network_status.server_list[block][i];
        s->online = ONLINE;
        s->used = USED;
        global_network_status.num_online_servers[block] = config.slot_config[slot][block];
    }
}

// Gestisce il processo di load balancing delle code
void load_balance(int block) {
    int slot = global_network_status.time_slot;
    int total_old = config.slot_config[slot - 1][block];
    int total_new = config.slot_config[slot][block];
    int total_job = blocks[block].jobInQueue;

    if (total_job == 0) {  // Non ci sono job da ri-distribuire
        return;
    }

    int only_new = total_new - total_old;
    int jobRemain = total_job / total_new;
    server last_server;

    int j = 0;
    int destID = 0;
    int lastID = 0;
    for (int i = 0; i < total_old; i++) {
        server *source = &global_network_status.server_list[block][i];
        while (source->jobInQueue > jobRemain) {
            destID = ((lastID) % (only_new)) + total_old;
            server *destination = &global_network_status.server_list[block][destID];

            struct job *tmp = source->tail->prev;
            enqueue_balancing(destination, source->tail);  // Sposta il job in coda dal servente source nella coda del servente di destinazione
            source->tail = tmp;
            if (source->tail->next) {
                source->tail->next = NULL;
            }

            destination->jobInTotal++;
            destination->arrivals++;
            if (destination->status == IDLE) {
                double serviceTime = getService(block, destination->stream);
                compl c = {destination, INFINITY};
                c.value = clock.current + serviceTime;
                destination->status = BUSY;
                insertSorted(&global_sorted_completions, c);
                blocks[block].jobInQueue--;  // Il primo job che andrà nel server IDLE APPENA ACCESSO non dovrà essere contato più come in coda, è in servizio
            } else {
                destination->jobInQueue++;  // Il numero di job in coda nel BLOCCO non varia perchè il job appena spostato si trovava comunque nella coda di un altro servente
            }
            source->jobInQueue--;
            source->jobInTotal--;
            source->arrivals--;
            lastID++;
        }
    }
}

// Aggiorna i serventi attivi al cambio di fascia, attivando o disattivando il numero necessario per ogni blocco
void update_network() {
    int actual, new = 0;
    int slot = global_network_status.time_slot;

    for (int j = 0; j < NUM_BLOCKS; j++) {
        actual = global_network_status.num_online_servers[j];
        new = config.slot_config[slot][j];
        if (actual > new) {
            deactivate_servers(j);
        } else if (actual < new) {
            activate_servers(j);
            if (slot != 0)
                load_balance(j);
        }
    }
}

// Cambia la fascia oraria settando il tasso di arrivo ed attivando/disattivando i server necessari
void set_time_slot(int rep) {
    if (clock.current == START && !slot_switched[0]) {
        global_network_status.time_slot = 0;
        arrival_rate = LAMBDA_1;
        slot_switched[0] = true;
        update_network();
    }
    if (clock.current >= TIME_SLOT_1 && clock.current < TIME_SLOT_1 + TIME_SLOT_2 && !slot_switched[1]) {
        if (rep == 0 && strcmp(simulation_mode, "FINITE") == 0) {
            print_p_on_csv(&global_network_status, clock.current, global_network_status.time_slot);
        }
        calculate_statistics_fin(&global_network_status, clock.current, response_times, global_means_p_fin, rep);
        global_network_status.time_slot = 1;
        arrival_rate = LAMBDA_2;
        slot_switched[1] = true;
        update_network();
    }

    if (clock.current >= TIME_SLOT_1 + TIME_SLOT_2 && !slot_switched[2]) {
        calculate_statistics_fin(&global_network_status, clock.current, response_times, global_means_p_fin, rep);
        if (rep == 0 && strcmp(simulation_mode, "FINITE") == 0) {
            print_p_on_csv(&global_network_status, clock.current, global_network_status.time_slot);
        }

        global_network_status.time_slot = 2;
        arrival_rate = LAMBDA_3;
        slot_switched[2] = true;
        update_network();
        print_servers_statistics(&global_network_status, clock.current, clock.current);
    }
}

// Inizializza lo stato dei blocchi del sistema
void init_network(int rep) {
    streamID = 0;
    clock.current = START;
    for (int i = 0; i < 3; i++) {
        slot_switched[i] = false;
        response_times[i] = 0;
    }

    init_blocks();
    if (str_compare(simulation_mode, "FINITE") == 0) {
        set_time_slot(rep);
    }

    completed = 0;
    bypassed = 0;
    dropped = 0;
    clock.arrival = getArrival(clock.current);
    global_sorted_completions.num_completions = 0;
}

// Inserisce un job nella coda del server specificato
void enqueue(server *s, double arrival) {
    struct job *j = (struct job *)malloc(sizeof(struct job));
    if (j == NULL)
        handle_error("malloc");

    j->arrival = arrival;
    j->next = NULL;

    // Appendi alla coda se esiste, altrimenti è la testa
    if (s->tail) {
        j->prev = s->tail;
        s->tail->next = j;  // aggiorno la vecchia coda e la faccio puntare a J
    } else {
        j->prev = NULL;
        s->head_service = j;
    }

    s->tail = j;  // J diventa la nuova coda
}

void enqueue_balancing(server *s, struct job *j) {
    if (s->tail != NULL) {  // Appendi alla coda se esiste, altrimenti è la testa
        s->tail->next = j;
        j->prev = s->tail;
    } else {
        s->head_service = j;
        if (s->head_service->prev)
            s->head_service->prev = NULL;
        if (s->head_service->next)
            s->head_service->next = NULL;
    }

    s->tail = j;
}

// Rimuove il job dalla coda del server specificato
void dequeue(server *s) {
    if (s->block->type == GREEN_PASS)
        return;

    struct job *j = s->head_service;

    if (!j->next)
        s->tail = NULL;

    s->head_service = j->next;
    free(j);
}

// Ritorna il server con meno job in coda di uno specifico blocco
server *findShorterServer(struct block b) {
    int block_type = b.type;
    int active_servers = global_network_status.num_online_servers[block_type];
    int init_server = Equilikely(0, global_network_status.num_online_servers[block_type] - 1);
    server *shorterTail = &global_network_status.server_list[block_type][init_server];

    // Nel blocco green pass non ci sono code, quindi bisogna trovare soltanto il server IDLE
    if (block_type == GREEN_PASS) {
        for (int j = 0; j < active_servers; j++) {
            if (global_network_status.server_list[block_type][j].status == IDLE) {
                return &global_network_status.server_list[block_type][j];
            }
        }
        return NULL;
    }

    int i = init_server;
    int n = 0;
    while (true) {
        if (n == active_servers)
            break;

        server s = global_network_status.server_list[block_type][i];
        int a = shorterTail->jobInTotal;
        if (s.jobInTotal < a) {
            if (!s.need_resched) {
                shorterTail = &global_network_status.server_list[block_type][i];
            }
        }
        n++;
        i = (i + 1) % active_servers;
    }
    return shorterTail;
}

// Processa un arrivo dall'esterno verso il sistema
void process_arrival() {
    blocks[TEMPERATURE_CTRL].total_arrivals++;

    server *s = findShorterServer(blocks[TEMPERATURE_CTRL]);
    s->jobInTotal++;
    s->arrivals++;

    // Se il server trovato non ha nessun job in servizio, può servire il job appena arrivato
    if (s->status == IDLE) {
        double serviceTime = getService(TEMPERATURE_CTRL, s->stream);
        compl c = {s, INFINITY};
        c.value = clock.current + serviceTime;
        s->status = BUSY;
        s->sum.service += serviceTime;
        s->sum.served++;
        insertSorted(&global_sorted_completions, c);
        enqueue(s, clock.arrival);
    } else {
        enqueue(s, clock.arrival);
        s->jobInQueue++;
        blocks[TEMPERATURE_CTRL].jobInQueue++;
    }

    clock.arrival = getArrival(clock.current);  // Genera prossimo arrivo
}

// Processa un next-event di completamento
void process_completion(compl c) {
    int block_type = c.server->block->type;

    blocks[block_type].total_completions++;
    c.server->completions++;
    c.server->jobInTotal--;

    blocks[block_type].jobInBlock--;

    int destination;
    server *shorterServer;

    dequeue(c.server);  // Toglie il job servito dal server e fa "avanzare" la lista collegata di job
    deleteElement(&global_sorted_completions, c);

    // Se nel server ci sono job in coda, devo generare il prossimo completamento per tale server.
    if (c.server->jobInQueue > 0) {
        c.server->jobInQueue--;
        blocks[block_type].jobInQueue--;
        double service_1 = getService(block_type, c.server->stream);
        c.value = clock.current + service_1;
        c.server->sum.service += service_1;
        c.server->sum.served++;
        insertSorted(&global_sorted_completions, c);

    } else {
        c.server->status = IDLE;
    }

    // Se un server è schedulato per la terminazione e non ha job in coda va offline
    if (c.server->need_resched && c.server->jobInQueue == 0) {
        c.server->online = OFFLINE;
        c.server->time_online += (clock.current - c.server->last_online);
        c.server->last_online = clock.current;
        c.server->need_resched = false;
    }

    // Se il completamento avviene sul blocco GREEN PASS allora il job esce dal sistema
    if (block_type == GREEN_PASS) {
        completed++;
        return;
    }

    // Gestione blocco destinazione
    destination = getDestination(c.server->block->type);  // Trova la destinazione adatta per il job appena servito
    if (destination == EXIT) {
        dropped++;
        blocks[TEMPERATURE_CTRL].total_bypassed++;
        return;
    }
    if (destination != GREEN_PASS) {
        blocks[destination].total_arrivals++;

        shorterServer = findShorterServer(blocks[destination]);
        shorterServer->arrivals++;
        shorterServer->jobInTotal++;
        enqueue(shorterServer, c.value);  // Posiziono il job nella coda del blocco destinazione e gli imposto come tempo di arrivo quello di completamento

        // Se il server trovato non ha nessuno in coda, generiamo un tempo di servizio
        if (shorterServer->status == IDLE) {
            compl c2 = {shorterServer, INFINITY};
            double service_2 = getService(destination, shorterServer->stream);
            c2.value = clock.current + service_2;
            insertSorted(&global_sorted_completions, c2);
            shorterServer->status = BUSY;
            shorterServer->sum.service += service_2;
            shorterServer->sum.served++;
            return;
        } else {
            shorterServer->jobInQueue++;
            blocks[destination].jobInQueue++;
            return;
        }
    }

    // Desination == GREEN_PASS. Se non ci sono serventi liberi il job esce dal sistema (loss system)
    blocks[destination].total_arrivals++;
    shorterServer = findShorterServer(blocks[destination]);

    if (shorterServer != NULL) {
        shorterServer->jobInTotal++;
        shorterServer->arrivals++;
        blocks[destination].jobInBlock++;
        compl c3 = {shorterServer, INFINITY};
        double service_3 = getService(destination, shorterServer->stream);
        c3.value = clock.current + service_3;
        insertSorted(&global_sorted_completions, c3);
        shorterServer->status = BUSY;
        shorterServer->sum.service += service_3;
        shorterServer->sum.served++;
        return;

    } else {
        completed++;
        bypassed++;
        blocks[GREEN_PASS].total_bypassed++;
        return;
    }
}

// Scrive i tempi di risposta a tempo finito su un file csv
void write_rt_csv_finite() {
    FILE *csv;
    char filename[100];
    for (int j = 0; j < 3; j++) {
        snprintf(filename, 100, "results/finite/rt_finite_slot%d.csv", j);
        csv = open_csv(filename);

        for (int i = 0; i < NUM_REPETITIONS; i++) {
            append_on_csv(csv, i, statistics[i][j], 0);
        }
        fclose(csv);
    }
}

// Stampa il costo e l'utilizzazione media ad orizzonte finito
void print_results_finite() {
    double total = 0;
    for (int i = 0; i < NUM_REPETITIONS; i++) {
        total += repetitions_costs[i];
    }

    printf("\nTOTAL MEAN CONFIGURATION COST: %f\n", total / NUM_REPETITIONS);
    for (int s = 0; s < 3; s++) {
        printf("\nSlot #%d:", s);
        for (int j = 0; j < NUM_BLOCKS; j++) {
            printf("\nMean Utilization for block %s: ", stringFromEnum(j));
            double p = 0;
            for (int i = 0; i < NUM_REPETITIONS; i++) {
                p += global_means_p_fin[i][s][j];
            }
            printf("%f", p / NUM_REPETITIONS);
        }
    }
}

// Scrive i tempi di risposta a tempo infinito su un file csv
void write_rt_csv_infinite(int slot) {
    char filename[100];
    snprintf(filename, 100, "results/infinite/rt_infinite_slot_%d.csv", slot);
    FILE *csv;
    csv = open_csv(filename);
    for (int j = 0; j < BATCH_K; j++) {
        append_on_csv(csv, j, infinite_statistics[j], 0);
    }
    fclose(csv);
}

// Stampa il costo e l'utilizzazione media ad orizzonte finito per ogni singolo server
void print_results_infinite(int slot) {
    double cost = calculate_cost(&global_network_status);
    printf("\n\nTOTAL SLOT %d CONFIGURATION COST: %f\n", slot, cost);

    double l = 0;
    for (int j = 0; j < NUM_BLOCKS; j++) {
        printf("\nMean Utilization for block %s: ", stringFromEnum(j));
        double p = 0;
        for (int i = 0; i < BATCH_K; i++) {
            p += global_means_p[i][j];
            if (j == GREEN_PASS) {
                l += global_loss[i];
            }
        }
        printf("%f", p / BATCH_K);
    }
    printf("\nGREEN PASS LOSS PERC %f: ", l / BATCH_K);
    printf("\n");
}

// Resetta le statistiche tra un batch ed il successivo
void reset_statistics() {
    clock.batch_current = clock.current;
    for (int block_type = 0; block_type < NUM_BLOCKS; block_type++) {
        blocks[block_type].total_arrivals = 0;
        blocks[block_type].total_completions = 0;
        blocks[block_type].total_bypassed = 0;
        blocks[block_type].area.node = 0;
        blocks[block_type].area.service = 0;
        blocks[block_type].area.queue = 0;
        for (int j = 0; j < MAX_SERVERS; j++) {
            server *s = &global_network_status.server_list[block_type][j];
            if (s->used) {
                s->arrivals = 0;
                s->completions = 0;
                s->area.node = 0;
                s->area.queue = 0;
                s->area.service = 0;
            }
        }
    }
}

// Setta la configurazione di avvio specificata
void init_config() {
    int slot_null[] = {0, 0, 0, 0, 0};

    // Configurazione di testing
    int slot_test_1[] = {7, 20, 2, 9, 11};
    int slot_test_2[] = {3, 11, 1, 3, 9};
    int slot_test_3[] = {14, 40, 3, 18, 20};
    int slot_test_4[] = {6, 18, 2, 10, 10};
    int slot_test_5[] = {9, 19, 3, 11, 15};

    // Configurazione non ottime in termini di costo
    int slot0_non_ottima[] = {12, 25, 4, 15, 20};
    int slot1_non_ottima[] = {18, 46, 5, 20, 25};
    int slot2_non_ottima[] = {10, 28, 5, 12, 15};

    // Configurazioni ottime
    int slot0_ottima[] = {8, 22, 2, 10, 11};
    int slot1_ottima[] = {15, 44, 3, 18, 20};
    int slot2_ottima[] = {7, 21, 2, 9, 10};

    // Configurazioni ottime del sistema base
    int slot0_ottima_orig[] = {8, 21, 2, 9, 11};
    int slot1_ottima_orig[] = {14, 41, 3, 17, 20};
    int slot2_ottima_orig[] = {8, 18, 2, 9, 10};

    // Configurazioni infinite
    int slot0_inf[] = {5, 19, 2, 9, 8};
    int slot1_inf[] = {8, 20, 2, 10, 20};
    int slot2_inf[] = {4, 13, 2, 6, 10};

    config = get_config(slot0_inf, slot1_inf, slot2_inf);
}