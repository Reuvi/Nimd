#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

#define QUEUE_SIZE 256
#define MAX_MESSAGE_LEN 104
#define MSG_HEADER_LEN 5
#define HOSTSIZE 100
#define PORTSIZE 10

#define RECV_OK        1   // return >0 for success (actual value = total bytes)
#define RECV_EOF       0   // clean EOF
#define RECV_SYSERR   -1   // read() error
#define RECV_BADFRAME -2   // malformed NGP framing


volatile int active = 1;

//Custom Extra Closers not in implementation
char *custom1 = "0|18|CONNECTION_FAILED|";
char *custom2 = "0|16|SERVER_SHUTDOWN|";

//This number is 
int max_games = 4;
int cur_game_index = -1;
pthread_mutex_t registry_lock;

enum State {
    AWAITING_SECOND_PLAYER,
    P1_TURN,
    P2_TURN,
    AWAITING_FIRST_PLAYER,
    GAME_START,
    GAME_OVER,
};

const char *state_to_str(int s) {
    switch (s) {
        case AWAITING_SECOND_PLAYER: 
            return "AWAITING_SECOND_PLAYER";
        case P1_TURN:               
            return "P1_TURN";
        case P2_TURN:               
            return "P2_TURN";
        case AWAITING_FIRST_PLAYER: 
            return "AWAITING_FIRST_PLAYER";
        case GAME_START:            
            return "GAME_START";
        case GAME_OVER:             
            return "GAME_OVER";
        default:                    
            return "UNKNOWN_STATE";
    }
}

//A players name has max 72 Characters, 73 used for identifying Null Term
//Board always has 5 stones
typedef struct {
    int p1_s; // Player 1 Socket
    int p2_s; // Player 2 Socket
    char p1_name[73]; // Player 1 Name
    char p2_name[73]; // Player 2 Name
    int board[5]; // Board State
    int state; // Game Session State
    pthread_mutex_t lock; // Mutex Lock for Game
    int index; // Index for game inside of Game Array
    pthread_t p1_t; // Thread for Player 1
    pthread_t p2_t; // Thread for Player 2
} Game;

typedef struct {
    int sock; // Player Sock
    struct sockaddr_storage rem; // Based on Class Code
    socklen_t rem_len; // Based on Class Code
    Game *session; // Ref to Game Session
} ConnArgs;

typedef struct {
    char *type;       // "OPEN" or "MOVE"
    char *fields[3];  // up to 3 fields (we only need up to 2)
    int field_count;
} ParsedMsg;

Game **sessions;

// Thread entry point, just wraps handle_connection for my args
// Also sets up a cleanup function if one thread had to cancel the other

void free_connargs(void *arg) {
    free(arg);
}

void handle_connection(int sock, struct sockaddr *rem, socklen_t rem_len, Game *session);

void *connection_thread(void *arg)
{
    ConnArgs *c = arg;
    pthread_cleanup_push(free_connargs, c);
    handle_connection(c->sock, (struct sockaddr *)&c->rem, c->rem_len, c->session);
    
    //If we return safely from the handle_connection then we should pop the cleanuphandler before returning
    pthread_cleanup_pop(1);
    return NULL;
}

// Extract payload from "id|len|payload|" into a modifiable buffer
int extract_payload(char *buf, char **payload_out, int *payload_len_out) {
    char *id_str = buf;
    char *bar1 = strchr(id_str, '|');
    if (!bar1) return -1;
    *bar1 = '\0';

    char *len_str = bar1 + 1;
    char *bar2 = strchr(len_str, '|');
    if (!bar2) return -1;
    *bar2 = '\0';

    // Require protocol id "0"
    if (strcmp(id_str, "0") != 0) return -1;

    // Enforce EXACTLY two digits for length
    if (strlen(len_str) != 2 || !isdigit((unsigned char)len_str[0]) || !isdigit((unsigned char)len_str[1]))
        return -1;

    int expected = (len_str[0] - '0') * 10 + (len_str[1] - '0');
    if (expected < 5 || expected > 104) return -1; // sanity

    char *payload = bar2 + 1;

    // Ensure buffer actually contains expected bytes
    if ((int)strlen(payload) < expected) return -1;

    // Payload must end with '|'
    if (payload[expected - 1] != '|') return -1;

    // Null-terminate right after payload so tokenizers are safe
    payload[expected] = '\0';

    *payload_out = payload;
    *payload_len_out = expected;
    return 0;
}

int parse_client_message(char *buf, ParsedMsg *out) {
    char *payload;
    int plen;

    if (extract_payload(buf, &payload, &plen) != 0) return -1;

    // payload includes trailing '|', length plen
    int bars = 0;
    for (int i = 0; i < plen; i++) if (payload[i] == '|') bars++;

    // Type must be 4 chars then '|'
    if (plen < 5) return -1;
    if (payload[4] != '|') return -1;

    // Split type (safe because we own buffer)
    payload[4] = '\0';
    out->type = payload;

    // Enforce allowed types early
    int expected_bars = -1;
    if (strcmp(out->type, "OPEN") == 0) expected_bars = 2;
    else if (strcmp(out->type, "MOVE") == 0) expected_bars = 3;
    else return -1;

    if (bars != expected_bars) return -1;

    // Now tokenize fields (no need to preserve empties because bar-count already enforced)
    char *saveptr = NULL;
    char *tok = strtok_r(payload + 5, "|", &saveptr); // after "TYPE\0"
    int idx = 0;

    while (tok != NULL) {
        if (idx >= 2) return -1; // too many fields for client messages
        out->fields[idx++] = tok;
        tok = strtok_r(NULL, "|", &saveptr);
    }
    out->field_count = idx;

    if (strcmp(out->type, "OPEN") == 0 && out->field_count != 1) return -1;
    if (strcmp(out->type, "MOVE") == 0 && out->field_count != 2) return -1;

    return 0;
}

// uses sessions[], cur_game_index, registry_lock from your file
static int name_in_use(const char *name) {
    int used = 0;

    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i <= cur_game_index && !used; i++) {
        Game *g = sessions[i];
        if (!g || g->state == AWAITING_FIRST_PLAYER) continue;

        pthread_mutex_lock(&g->lock);
        if (g->p1_s != -1 &&
            g->p1_name[0] &&
            strcmp(g->p1_name, name) == 0) {
            used = 1;
        } else if (g->p2_s != -1 &&
                   g->p2_name[0] &&
                   strcmp(g->p2_name, name) == 0) {
            used = 1;
        }

        pthread_mutex_unlock(&g->lock);
    }
    pthread_mutex_unlock(&registry_lock);

    return used;
}

//Reset a Game State that was game Over'ed
void resetGame(Game *g)
{
    for (int i = 0; i < 5; i++) {
        g->board[i] = 2 * i + 1;
    }
    g->p1_s = -1;
    g->p2_s = -1;
    g->p1_name[0] = '\0';
    g->p2_name[0] = '\0';
    g->state = AWAITING_FIRST_PLAYER;
    g->p1_t = 0;
    g->p2_t = 0;
}


//Intiallize Game
void gameInit(Game *session)
{
    for (int i = 0; i < 5; i++) {
        session->board[i] = 2 * i + 1;
    }

    session->p1_s = -1;
    session->p2_s = -1;
    session->state = AWAITING_FIRST_PLAYER;

    session->p1_name[0] = '\0';
    session->p2_name[0] = '\0';

    pthread_mutex_init(&session->lock, NULL);
}

/*
DEPRECATED FUNCTION 
int gameDestroy(Game ***sessions, int index) 
{ 
    pthread_mutex_lock(&registry_lock); 
    if (index < 0 || index > cur_game_index) 
    { 
        pthread_mutex_unlock(&registry_lock); 
        return 1; 
    } 
    Game *session = (*sessions)[index]; 
    // Tear down the games data and mutexes 
    pthread_mutex_destroy(&session->lock); 
    free(session); // move last game into this slot if needed 
    // doesnt matter if last games also dies at same time bc it will have to wait 
    // From the registery mutex 
    if (index != cur_game_index) 
    { 
        (*sessions)[index] = (*sessions)[cur_game_index]; 
        (*sessions)[index]->index = index; 
    } 
    cur_game_index--; 
    
    //Optionally shrink our registry for efficiency 
    int used = cur_game_index + 1; 
    if (used <= max_games / 2 && max_games > 4) 
    { 
        int new_max = max_games / 2; 
        Game **tmp = realloc(*sessions, new_max * sizeof(Game *)); 
        if (tmp != NULL) 
            { 
                *sessions = tmp; 
                max_games = new_max; 
            } 
        else { 
            pthread_mutex_unlock(&registry_lock); return 1; 
            } 
    } 
    pthread_mutex_unlock(&registry_lock); 
    return 0; 
}

*/

void formatOver(char *buf, int forfeit, int winner, int *board) {
    char payload[64];
    int pos = 0;

    pos += sprintf(payload + pos, "OVER|%d|", winner);

    for (int i = 0; i < 5; i++) {
        pos += sprintf(payload + pos, "%d", board[i]);
        if (i < 4) {
            payload[pos++] = ' ';
            payload[pos] = '\0';
        }
    }

    pos += sprintf(payload + pos, "|");

    if (forfeit) {
        pos += sprintf(payload + pos, "Forfeit|");
        
    } else {
        pos += sprintf(payload + pos, "|");
    }

    int payload_len = pos;

    sprintf(buf, "0|%02d|%s", payload_len, payload);
}

// FAIL|code|msg|
void formatFail(char *buf, int code, const char *msg) {
    char payload[80];
    int pos = 0;
    pos += sprintf(payload + pos, "FAIL|%d %s|", code, msg);
    int payload_len = pos;
    sprintf(buf, "0|%02d|%s", payload_len, payload);
}

void formatWait(char *buf) {
    const char *payload = "WAIT|";
    sprintf(buf, "0|0%zu|%s", strlen(payload), payload);
}

// NAME|player_num|opponent|p1 p2 p3 p4 p5|
void formatName(char *buf, int player_num, const char *opponent, int *board) {
    char payload[128];
    int pos = 0;

    pos += sprintf(payload + pos, "NAME|%d|%s|", player_num, opponent);

    /*
    for (int i = 0; i < 5; i++) {
        pos += sprintf(payload + pos, "%d", board[i]);
        if (i < 4) {
            payload[pos++] = ' ';
            payload[pos] = '\0';
        }
    }
    pos += sprintf(payload + pos, "|");
    */

    int payload_len = pos;
    sprintf(buf, "0|%02d|%s", payload_len, payload);
}

// PLAY|whose_turn|p1 p2 p3 p4 p5|
static void formatPlay(char *buf, int whose_turn, int *board) {
    char payload[96];
    int pos = 0;

    pos += sprintf(payload + pos, "PLAY|%d|", whose_turn);
    for (int i = 0; i < 5; i++) {
        pos += sprintf(payload + pos, "%d", board[i]);
        if (i < 4) {
            payload[pos++] = ' ';
            payload[pos] = '\0';
        }
    }
    pos += sprintf(payload + pos, "|");

    int payload_len = pos;
    sprintf(buf, "0|%02d|%s", payload_len, payload);
}

static void send_fail_and_maybe_forfeit(Game *session, int sock, int player, int code, const char *msg, int *bytes_ptr)
{
    char buf[MAX_MESSAGE_LEN + 1];
    formatFail(buf, code, msg);
    write(sock, buf, strlen(buf));

    pthread_mutex_lock(&session->lock);

    // Only forfeit if we’re actually in a playing state
    if (session->state == P1_TURN || session->state == P2_TURN) {
        int loser  = player;
        int winner = (player == 1) ? 2 : 1;

        int loser_sock  = (loser  == 1) ? session->p1_s : session->p2_s;
        int winner_sock = (winner == 1) ? session->p1_s : session->p2_s;

        // Send OVER to the winner
        if (winner_sock != -1) {
            char over_buf[MAX_MESSAGE_LEN + 1];
            formatOver(over_buf, 1, winner, session->board);
            write(winner_sock, over_buf, strlen(over_buf));

            // Wake up winner thread's read() so it can hit cleanup and close
            shutdown(winner_sock, SHUT_RDWR);
        }

        // Also wake up loser thread's read() (this same sock or the other one)
        if (loser_sock != -1 && loser_sock != winner_sock) {
            shutdown(loser_sock, SHUT_RDWR);
        }

        session->state = GAME_OVER;
    }

    pthread_mutex_unlock(&session->lock);

    // And ensure THIS thread’s recv loop sees EOF / error
    shutdown(sock, SHUT_RDWR);
    if (bytes_ptr) *bytes_ptr = 0;  // so your cleanup sees bytes == 0 if you use that
}


static void maybe_start_game(Game *session) {
    pthread_mutex_lock(&session->lock);
    if (session->state == GAME_START &&
        session->p1_name[0] != '\0' && session->p2_name[0] != '\0') {

        // starting piles: 1 3 5 7 9
        for (int i = 0; i < 5; i++) {
            session->board[i] = 2 * i + 1;
        }

        session->state = P1_TURN;

        char name1[MAX_MESSAGE_LEN + 1];
        char name2[MAX_MESSAGE_LEN + 1];
        char play[MAX_MESSAGE_LEN + 1];

        formatName(name1, 1, session->p2_name, session->board);
        formatName(name2, 2, session->p1_name, session->board);
        formatPlay(play, 1, session->board);

        if (session->p1_s != -1) {
            write(session->p1_s, name1, strlen(name1));
            write(session->p1_s, play,  strlen(play));
        }
        if (session->p2_s != -1) {
            write(session->p2_s, name2, strlen(name2));
            write(session->p2_s, play,  strlen(play));
        }

        printf("[GAME %d] Starting game: P1='%s' P2='%s'\n", session->index, session->p1_name, session->p2_name);
        printf("[GAME %d] Initial board: %d %d %d %d %d\n", session->index, session->board[0], session->board[1], session->board[2], session->board[3], session->board[4]);
        printf("[GAME %d] -> NAME to P1, NAME to P2, then PLAY whose_turn=1\n", session->index);

    }
    pthread_mutex_unlock(&session->lock);
}

int recv_ngp_message(int sock, char *buf, size_t bufsize)
{
    size_t total = 0;

    // 1) Read "id|"  (we don't care what id is right now)
    while (1) {
        char c;
        ssize_t n = read(sock, &c, 1);
        if (n == 0) {
            return RECV_EOF;    // connection closed
        } else if (n < 0) {
            return RECV_SYSERR; // read error
        }

        if (total + 1 >= bufsize) {
            return RECV_BADFRAME; // header too long for buffer
        }

        buf[total++] = c;

        if (c == '|') {
            break; // finished id field
        }
    }

    // 2) Read "<len>|"  (decimal length until next '|')
    size_t len_start = total;
    int have_digit = 0;

    while (1) {
        char c;
        ssize_t n = read(sock, &c, 1);
        if (n == 0) {
            return RECV_EOF;
        } else if (n < 0) {
            return RECV_SYSERR;
        }

        if (total + 1 >= bufsize) {
            return RECV_BADFRAME; // header too long for buffer
        }

        buf[total++] = c;

        if (c == '|') {
            break; // end of length field
        }
        if (!isdigit((unsigned char)c)) {
            return RECV_BADFRAME; // length field must be digits
        }
        have_digit = 1;
    }

    if (!have_digit) {
        return RECV_BADFRAME; // empty length field
    }

    //Must be two digits wide
    if ((total - len_start - 1) != 2)  // -1 for end '|'
        return RECV_BADFRAME;

    // Parse length: from len_start up to (total - 1), since total-1 is the '|'
    int msg_len = 0;
    for (size_t i = len_start; i < total - 1; i++) {
        msg_len = msg_len * 10 + (buf[i] - '0');
        if (msg_len < 0) {
            return RECV_BADFRAME; // overflow or nonsense
        }
    }
    if (msg_len <= 0) {
        return RECV_BADFRAME;
    }

    if ((size_t)msg_len + total >= bufsize) {
        // not enough room in buffer for payload + '\0'
        return RECV_BADFRAME;
    }

    // 3) Read exactly msg_len payload bytes
    size_t need = (size_t)msg_len;
    while (need > 0) {
        ssize_t n = read(sock, buf + total, need);
        if (n == 0) {
            return RECV_EOF;    // EOF mid-message
        } else if (n < 0) {
            return RECV_SYSERR; // read error
        }
        total += (size_t)n;
        need  -= (size_t)n;
    }

    buf[total] = '\0';

    // 4) Spec requires payload end with '|' terminator
    if (buf[total - 1] != '|') {
        return RECV_BADFRAME;
    }

    // Success: total = header + payload bytes
    return (int)total;
}


//Either Adds a game OR switches the game context to a previous stuck state I.E. someone waiting prior. Games that are ended are ended
// Yes I know it O(N) time but I do not want to rewrite my code
int addGame(Game ***sessions){
    
    pthread_mutex_lock(&registry_lock);

    for (int i = 0; i <= cur_game_index; i++) {
        Game *g = (*sessions)[i];
        if (!g) continue;

        if (g->state == AWAITING_FIRST_PLAYER || g->state == AWAITING_SECOND_PLAYER || g->state == GAME_OVER) 
        {
        // Found a waiting game.
        // We want this game to become the "front" game at cur_game_index,
        // since main() will use sessions[cur_game_index] for the new connection.
            printf("[REGISTRY] Reusing game %d in state %s\n", g->index, state_to_str(g->state));

            if (g->state == GAME_OVER) {
                pthread_mutex_lock(&g->lock);
                printf("[REGISTRY] Resetting GAME_OVER game %d\n", g->index);
                resetGame(g);
                pthread_mutex_unlock(&g->lock);
            }

            if (i != cur_game_index) {

                printf("[REGISTRY] Swapped game %d with game %d; cur_game_index=%d\n", i, cur_game_index, cur_game_index);

                Game *cur = (*sessions)[cur_game_index];

                // swap positions
                (*sessions)[cur_game_index] = g;
                g->index = cur_game_index;

                (*sessions)[i] = cur;
                cur->index = i;
            }

            pthread_mutex_unlock(&registry_lock);
            return 0;
        }
}

    if (cur_game_index == max_games - 1) {
        printf("[REGISTRY] Resized sessions: old max=%d new max=%d\n", max_games / 2, max_games);

        int new_max = max_games * 2;
        Game **tmp = realloc(*sessions, new_max * sizeof(Game *));
        if(tmp == NULL) {
            pthread_mutex_unlock(&registry_lock);
            return 1;
        }
        *sessions = tmp;
        max_games = new_max;
    }

    Game *newSession = malloc(sizeof(Game));
    if(newSession == NULL) {
        pthread_mutex_unlock(&registry_lock);
        return 1;
    }

    gameInit(newSession);
    
    cur_game_index += 1;
    newSession->index = cur_game_index;
    
    printf("[REGISTRY] Created new game %d; total games now: %d (max_games=%d)\n", newSession->index, cur_game_index + 1, max_games);

    (*sessions)[cur_game_index] = newSession;

    pthread_mutex_unlock(&registry_lock);

    return 0;
}

//free one game at index
void gameDestroyOne(int index) {
    Game *session = sessions[index];
    
    printf("[REGISTRY] Destroying game %d\n", index);

    if (!session) return;

    pthread_mutex_destroy(&session->lock);
    free(session);
    sessions[index] = NULL;
}


//For Graceful shutdowns
void handler(int signum)
{
    active = 0;
}

void
install_handlers(void)
{
    struct sigaction act;
    act.sa_handler = handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGINT);
    sigaddset(&act.sa_mask, SIGTERM);

    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
}

// Handles Game Connections per Socket
//One of the two connections is responsible for starting the game for the players
void handle_connection(int sock, struct sockaddr *rem, socklen_t rem_len, Game *session)
{
    char buf[MAX_MESSAGE_LEN + 1], host[HOSTSIZE], port[PORTSIZE];
    int bytes = 0, error;
    int have_open = 0;  // has this client sent a successful OPEN?

    error = getnameinfo(rem, rem_len, host, HOSTSIZE, port, PORTSIZE, NI_NUMERICSERV);
    if (error) {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(error));
        strcpy(host, "??");
        strcpy(port, "??");
    }

    printf("[GAME %d] New connection thread started for socket %d from %s:%s\n", session->index, sock, host, port);

    while (active) {
        
        bytes = recv_ngp_message(sock, buf, sizeof(buf));

          // Figure out if this socket is currently player 1 or 2 (handles the rare remap case)
        int player = 0;
        pthread_mutex_lock(&session->lock);
        if (sock == session->p1_s) player = 1;
        else if (sock == session->p2_s) player = 2;
        pthread_mutex_unlock(&session->lock);

        if (player == 0) {
            // Socket no longer belongs to this game
            bytes = 0;
            break;
        }

        // After determining 'player' (1 or 2)
        printf("[GAME %d] Socket %d identified as Player %d (state=%s)\n", session->index, sock, player, state_to_str(session->state));

        if (bytes == RECV_EOF || bytes == RECV_SYSERR) {
            // normal cleanup will handle this
            break;
        }
        if (bytes == RECV_BADFRAME) {
            
            if (player != 0) {
                send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", NULL);
            }
            bytes = 0; // so cleanup code treats as EOF/close
            break;
        }
        buf[bytes] = '\0';
        printf("[%s:%s] read %d bytes {%s} | Game Index [%d] \n", host, port, bytes, buf, session->index);

        ParsedMsg msg;
        if (parse_client_message(buf, &msg) != 0) {
            // FAIL 10 Invalid, and if game started, opponent wins by forfeit
            send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", &bytes);
            break;
        }

        printf("[GAME %d][P%d] Received type=%s with %d field(s)\n", session->index, player, msg.type, msg.field_count);
        for (int i = 0; i < msg.field_count; i++) {
            printf("    field[%d] = '%s'\n", i, msg.fields[i]);
        }


        // ---------- FIRST MESSAGE MUST BE OPEN ----------
        if (!have_open) {
            if (strcmp(msg.type, "OPEN") != 0) {
                // First valid payload but not OPEN -> FAIL 24 Not Playing
                send_fail_and_maybe_forfeit(session, sock, player, 24, "Not Playing", &bytes);
                break;
            }

            if (msg.field_count < 1 || !msg.fields[0]) {
                send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", &bytes);
                break;
            }

            char *name = msg.fields[0];
            size_t name_len = strlen(name);
            if (name_len == 0 || name_len > 72) {
                // FAIL 21 Long Name
                send_fail_and_maybe_forfeit(session, sock, player, 21, "Long Name", &bytes);
                break;
            }

            // Already in another game? → FAIL 22 Already Playing
            if (name_in_use(name)) {
                send_fail_and_maybe_forfeit(session, sock, player, 22, "Already Playing", &bytes);
                break;
            }

            // Store the name into the Game
            pthread_mutex_lock(&session->lock);
            if (player == 1) {
                strncpy(session->p1_name, name, 72);
                session->p1_name[72] = '\0';
            } else {
                strncpy(session->p2_name, name, 72);
                session->p2_name[72] = '\0';
            }
            pthread_mutex_unlock(&session->lock);

            // Send WAIT| back
            char wait_msg[MAX_MESSAGE_LEN + 1];
            formatWait(wait_msg);
            write(sock, wait_msg, strlen(wait_msg));

            printf("[GAME %d][P%d] -> WAIT\n", session->index, player);


            have_open = 1;

            // If this completes both names and state == GAME_START, start the game
            maybe_start_game(session);
            continue;
        }

        // ---------- AFTER OPEN: either MOVE or protocol fail ----------

        if (strcmp(msg.type, "OPEN") == 0) {
            // Second OPEN -> FAIL 23 Already Open, then drop; if game started, opponent wins
            send_fail_and_maybe_forfeit(session, sock, player, 23, "Already Open", &bytes);
            break;
        }

        if (strcmp(msg.type, "MOVE") != 0) {
            // Unknown type -> FAIL 10 Invalid
            send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", &bytes);
            break;
        }

        // MOVE requires two integer fields: pile, qty
        if (msg.field_count < 2 || !msg.fields[0] || !msg.fields[1]) {
            send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", &bytes);
            break;
        }

        char *pile_str = msg.fields[0];
        char *qty_str  = msg.fields[1];
        char *endp;

        long pile = strtol(pile_str, &endp, 10);
        if (*endp != '\0') {
            send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", &bytes);
            break;
        }
        long qty = strtol(qty_str, &endp, 10);
        if (*endp != '\0') {
            send_fail_and_maybe_forfeit(session, sock, player, 10, "Invalid", &bytes);
            break;
        }

        pthread_mutex_lock(&session->lock);
        int state = session->state;

        printf("[GAME %d][P%d] MOVE request: pile=%ld qty=%ld (state=%s)\n", session->index, player, pile, qty, state_to_str(state));

        // If game isn't actually in a playing state -> FAIL 24 Not Playing
        if (state != P1_TURN && state != P2_TURN) {
            pthread_mutex_unlock(&session->lock);
            send_fail_and_maybe_forfeit(session, sock, player, 24, "Not Playing", &bytes);
            break;
        }

        int expected_player = (state == P1_TURN) ? 1 : 2;
        if (player != expected_player) {
            // Wrong turn -> FAIL 31 Impatient, but game continues
            pthread_mutex_unlock(&session->lock);
            char fbuf[MAX_MESSAGE_LEN + 1];
            formatFail(fbuf, 31, "Impatient");
            write(sock, fbuf, strlen(fbuf));

            printf("[GAME %d][P%d] Invalid MOVE -> FAIL %d (%s)\n", session->index, player, 31, "Impatient");

            continue;
        }

        // Pile index check
        if (pile < 1 || pile > 5) {
            pthread_mutex_unlock(&session->lock);
            char fbuf[MAX_MESSAGE_LEN + 1];
            formatFail(fbuf, 32, "Pile Index");
            write(sock, fbuf, strlen(fbuf));

            printf("[GAME %d][P%d] Invalid MOVE -> FAIL %d (%s)\n", session->index, player, 32, "Pile Index");

            continue;
        }

        int idx = (int)pile - 1;

        // Quantity check
        if (qty < 1 || qty > session->board[idx]) {
            pthread_mutex_unlock(&session->lock);
            char fbuf[MAX_MESSAGE_LEN + 1];
            formatFail(fbuf, 33, "Quantity");
            write(sock, fbuf, strlen(fbuf));

            printf("[GAME %d][P%d] Invalid MOVE -> FAIL %d (%s)\n", session->index, player, 33, "Quantity");

            continue;
        }

        // Apply the move
        session->board[idx] -= (int)qty;

        int sum = 0;
        for (int i = 0; i < 5; i++) {
            sum += session->board[i];
        }

        if (sum == 0) {
            int winner = player;

            char over_buf[MAX_MESSAGE_LEN + 1];
            formatOver(over_buf, 0, winner, session->board); // forfeit=0

            int p1 = session->p1_s;
            int p2 = session->p2_s;

            // Send OVER to both players (if they exist)
            if (p1 != -1) {
                write(p1, over_buf, strlen(over_buf));
            }
            if (p2 != -1 && p2 != p1) {
                write(p2, over_buf, strlen(over_buf));
            }

            printf("[GAME %d] Normal win by P%d. Sending OVER to both.\n", session->index, winner);

            // Mark game over under the lock
            session->state = GAME_OVER;

            if (p1 != -1) {
                shutdown(p1, SHUT_RDWR);
            }
            if (p2 != -1 && p2 != p1) {
                shutdown(p2, SHUT_RDWR);
            }

            pthread_mutex_unlock(&session->lock);

            // this thread also exits the recv loop cleanly
            bytes = 0;   // cleanup sees "EOF-ish"
            break;    
        } else {
            // Game continues, swap turn
            int next = (player == 1) ? 2 : 1;
            session->state = (next == 1) ? P1_TURN : P2_TURN;

            char play_buf[MAX_MESSAGE_LEN + 1];
            formatPlay(play_buf, next, session->board);

            if (session->p1_s != -1) write(session->p1_s, play_buf, strlen(play_buf));
            if (session->p2_s != -1) write(session->p2_s, play_buf, strlen(play_buf));

            printf("[GAME %d] -> PLAY whose_turn=%d board=%d %d %d %d %d\n", session->index, next, session->board[0], session->board[1], session->board[2], session->board[3], session->board[4]);

            pthread_mutex_unlock(&session->lock);
            continue;
        }
    }

    // Here we handle when the game closes
    // Either we sigInt, or a player disconnected, or game ends normally
    //Lock so only one of the two games handles this
    pthread_mutex_lock(&session->lock);
    if (session->state == GAME_OVER) {
        close(sock);
        if (sock == session->p1_s) {
            session->p1_s = -1;
        } else if (sock == session->p2_s) {
            session->p2_s = -1;
        }
        pthread_mutex_unlock(&session->lock);
        return;
    }
    //If anyone tried to cancel, cancel me now edge cases in shutdowns
    //pthread_testcancel();
    
    // The first thing we always do in these situations is kill the other thread;

    printf("[GAME %d] Cleanup for socket %d: bytes=%d, state=%s\n", session->index, sock, bytes, state_to_str(session->state));

    if (bytes == 0) {
        if (session->state == AWAITING_SECOND_PLAYER) {
            // Means their was only one player in the game
            // We can just remove the player and do nothing
            session->state = AWAITING_FIRST_PLAYER;

            if (sock == session->p1_s) {
                session->p1_s = -1;
                session->p1_name[0] = '\0';
                session->p1_t = 0;
            }
            
        } else if (session->state == GAME_START) {
            // This is the very RARE case where this happens
            /*
            player 1 Connects ->
            player 1 sends name ->
            <- wait player 1
            player 2 Connects ->
            player 1 disconnects ->
            Game state is in GAME_START but player 1 disconnected
            move player 2 to player 1, change the GAME_STAT,
            player 1(2) Sends Name ->
            <- wait player 1(2)
            */

            if (sock == session->p1_s) {
                // move socket / thread
                session->p1_s = session->p2_s;
                session->p1_t = session->p2_t;

                // safely move name p2 -> p1
                memmove(session->p1_name, session->p2_name, sizeof(session->p1_name));

                // make sure it's null-terminated
                session->p1_name[sizeof(session->p1_name) - 1] = '\0';

                //clear p2_name
                session->p2_name[0] = '\0';
            }
            session->state = AWAITING_SECOND_PLAYER;
        }
        else {
            // Otherwise we need to Forfeit the game because the game started
            // and the players recieved their names

            printf("[GAME %d] Socket %d disconnected; treating as forfeit.\n", session->index, sock);

            if (sock == session->p1_s) {
                // Player 1 disconnected so send player 2 info and cancel thread
                pthread_cancel(session->p2_t);
                formatOver(buf, 1, 2, session->board);
                write(session->p2_s, buf, strlen(buf));
                close(session->p2_s);
            } else {
                //Player 2 disconnected so send player 1 info and cancel thread
                pthread_cancel(session->p1_t);
                formatOver(buf, 1, 1, session->board);
                write(session->p1_s, buf, strlen(buf));
                close(session->p1_s);
            }

            //Shut down this Game
            //gameDestroy(&sessions, session->index);
            session->state = GAME_OVER;
        }
        printf("[%s:%s] got EOF\n", host, port);

    } else if (bytes == -1) {
        //Read Failed treat as Connection failed for both Players and handle both not for official submission
        //if (session->p1_s != -1) write(session->p1_s, custom1, strlen(custom1));
        //if (session->p2_s != -1) write(session->p2_s, custom1, strlen(custom1));

        printf("[GAME %d] Read error on socket %d: %s\n", session->index, sock, strerror(errno));


        if (sock == session->p1_s && session->p2_s != -1) {
            // only if P2 actually existed
            pthread_cancel(session->p2_t);
            close(session->p2_s);
        } else if (sock == session->p2_s && session->p1_s != -1) {
            pthread_cancel(session->p1_t);
            close(session->p1_s);
        }

        //gameDestroy(&sessions, session->index);
        session->state = GAME_OVER;
        printf("[%s:%s] failed to read, sending connection failure: %s\n", host, port, strerror(errno));
    } else {
        //if (session->p1_s != -1) write(session->p1_s, custom2, strlen(custom2));
        //if (session->p2_s != -1) write(session->p2_s, custom2, strlen(custom2));
        // Not for official submission only for my modified version
        printf("[GAME %d] Sending SERVER_SHUTDOWN to remaining players.\n", session->index);

        if (sock == session->p1_s && session->p2_s != -1) {
            // only if P2 actually existed
            pthread_cancel(session->p2_t);
            close(session->p2_s);
        } else if (sock == session->p2_s && session->p1_s != -1) {
            pthread_cancel(session->p1_t);
            close(session->p1_s);
        }

        //gameDestroy(&sessions, session->index);
        session->state = GAME_OVER;
        printf("[%s:%s] terminating, sending SERVER SHUTDOWN: %s\n", host, port, strerror(errno));
    }
    
    //Dont need to unlock it because its literally destroyed and never
    //Used again
    close(sock);
    if (sock == session->p1_s) {
        session->p1_s = -1;
    } else if (sock == session->p2_s) {
        session->p2_s = -1;
    }
    pthread_mutex_unlock(&session->lock);
    return;
}

int 
open_listener(char *service, int queue_size)
{
    struct addrinfo hint, *info_list, *info;
    int error, sock;

    //initialize hints
    memset(&hint, 0, sizeof(struct addrinfo));
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_PASSIVE;
    
    //obtain information for listening socket
    error = getaddrinfo(NULL, service, &hint, &info_list);
    if (error) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }

    //attempt to create socket
    for (info = info_list; info != NULL; info = info->ai_next) {
        sock = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        
        // if we could not create the socket, try the next method (We are guessting which info we need)
        if (sock == -1) continue;

        int yes = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
            perror("setsockopt(SO_REUSEADDR)");
            close(sock);
            continue;
        }


        // bind socket to request port
        error = bind(sock, info->ai_addr, info->ai_addrlen);
        if (error) {
            perror("bind");
            close(sock);
            continue;
        }


        // enable listening for incoming connection requests
        //Posibly update code here for dynamuc queue size?
        error = listen(sock, queue_size);
        if (error) {
            close(sock);
            continue;
        }

        // if we got this far we have opened the socket
        break;
    }

    freeaddrinfo(info_list);

    //info will be NULL if no method succeeded
    if (info == NULL) {
        fprintf(stderr, "Could not bind\n");
        return -1;
    }

    return sock;
}

int
main(int argc, char** argv) 
{
    if (argc != 2) {
        fprintf(stderr, "Usage: ./nimd [PORT]\n");
        return EXIT_FAILURE;
    }

    struct sockaddr_storage remote_host;
    socklen_t remote_host_len;

    char *PORT = argv[1];

    signal(SIGPIPE, SIG_IGN);
 
    //This allows us to have a graceful shutdown from all our threads if we do a control C
    install_handlers();
    pthread_mutex_init(&registry_lock, NULL);

    //Add our first game
    sessions = malloc(max_games * sizeof(Game *));
    if (addGame(&sessions)) {
        fprintf(stderr, "Failed to initalize first game session.");
        return EXIT_FAILURE;
    }

    int listener = open_listener(PORT, QUEUE_SIZE);
    if (listener < 0) exit(EXIT_FAILURE);

    printf("Listening for incoming connections on %s\n", PORT);

    while (active) {
        remote_host_len = sizeof(remote_host);
        int sock = accept(listener, (struct sockaddr *)&remote_host, &remote_host_len);

        if (sock < 0) {
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&registry_lock);
        Game *session = sessions[cur_game_index];
        
        printf("[MAIN] Accepted socket %d; attached to game %d (state=%s)\n", sock, session->index, state_to_str(session->state));
        
        pthread_mutex_unlock(&registry_lock);

        pthread_mutex_lock(&session->lock);

        if (session->state == P1_TURN || session->state == P2_TURN || session->state == GAME_START || session->state == GAME_OVER) {
            
            printf("[MAIN] Current front game %d is busy (state=%s), creating/reusing new game\n", session->index, state_to_str(session->state));

            pthread_mutex_unlock(&session->lock);

            //If adding a game fails
            if (addGame(&sessions)) {
                //Send Close to Socket and Ask it to Reconnect
                write(sock, custom1, strlen(custom1));
                close(sock);
                continue;
            };

            // Use the newly created game or reused game
            pthread_mutex_lock(&registry_lock);
            session = sessions[cur_game_index];
            
            printf("[MAIN] Using game %d for new connection\n", session->index);

            pthread_mutex_unlock(&registry_lock);

            pthread_mutex_lock(&session->lock);
        }

        // Build thread args
        ConnArgs *args = malloc(sizeof(ConnArgs));
        if (args == NULL) {
            pthread_mutex_unlock(&session->lock);
            write(sock, custom1, strlen(custom1));
            close(sock);
            continue;
        }

        args->sock = sock;
        args->rem_len = remote_host_len;
        memcpy(&args->rem, &remote_host, remote_host_len);
        args->session = session;

        if (session->state == AWAITING_FIRST_PLAYER) {
            if (pthread_create(&session->p1_t, NULL, connection_thread, args) != 0) {
                perror("pthread_create");
                free(args);
                write(sock, custom1, strlen(custom1));
                close(sock);
                pthread_mutex_unlock(&session->lock);
                continue;
            }
            pthread_detach(session->p1_t);
            session->p1_s = sock;
            session->state = AWAITING_SECOND_PLAYER;
        } else if (session->state == AWAITING_SECOND_PLAYER) {
            if (pthread_create(&session->p2_t, NULL, connection_thread, args) != 0) {
                perror("pthread_create");
                free(args);
                write(sock, custom1, strlen(custom1));
                close(sock);
                pthread_mutex_unlock(&session->lock);
                continue;
            }
            pthread_detach(session->p2_t);
            // Game is ready to start
            session->p2_s = sock;
            session->state = GAME_START;
        } else {
            // Should not happen but just in case
            free(args);
            write(sock, custom1, strlen(custom1));
            close(sock);
        }
        pthread_mutex_unlock(&session->lock);
    }

    fprintf(stdout, "[SHUTDOWN]|Shut down server from signal.\n");
    close(listener);

    for (int i = 0; i <= cur_game_index; i++) {
        gameDestroyOne(i);
    }
    free(sessions);

    printf("[MAIN] Server shutdown complete. Freed %d game(s).\n", cur_game_index + 1);

    return EXIT_SUCCESS;

}