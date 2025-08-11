// ============================================================================
// Autore: de Dato A. – Matricola: 635256
//
// SERVER – Quiz a temi con:
//  - Stampa stato (test disponibili, utenti online con test svolti, classifiche)
//  - Classifiche ordinate per punteggio e, a parità, per tempo di completamento
//  - Shutdown controllato da tastiera: premere 'Q' + Invio per spegnere il server
//
// ============================================================================

#include "utility.h"      // costanti, tipi e utility comuni
#include <pthread.h>      // thread POSIX
#include <dirent.h>       // lettura directory (qa/)
#include <netinet/in.h>   // sockaddr_in, htons
#include <time.h>         // time(), localtime_r, strftime
#include <stdatomic.h>    // atomic_int per shutdown cooperativo
#include <ctype.h>        // tolower, isspace

// ==================== Configurazione ====================
#define MAX_THREAD   8          // max client simultanei (1 slot per thread)
#define QA_FOLDER    "qa/"      // cartella con i file .txt (uno per tema)
#define SERVER_PORT  4242       // porta TCP del server

// ==================== Strutture Dati ====================

/*
 * GiocatoreStato
 *  - Mantiene lo stato dell'utente collegato nel thread/slot.
 *    nome[0] == '\0' => slot libero (nessuno connesso).
 *    temaCorr == NULL => non sta svolgendo un quiz in questo istante.
 */
struct GiocatoreStato {
    char  nome[MaxUsernameL];   // nickname corrente
    char* temaCorr;             // nome del tema in corso (puntatore in temiQuiz)
};

/*
 * NodoPunteggio
 *  - Nodo di lista doppiamente concatenata per la classifica di un tema.
 *  - finito: timestamp (secondi epoch) di fine quiz per tie-break;
 *            0 quando il quiz non è stato ancora completato.
 *  - nick:   COPIA del nickname (decoupling dagli slot online)
 */
struct NodoPunteggio {
    unsigned int           punteggio;           // risposte corrette accumulate
    time_t                 finito;              // istante di fine quiz (per il tie-break)
    char                   nick[MaxUsernameL];
    struct NodoPunteggio*  prev;                // nodo precedente
    struct NodoPunteggio*  nxt;                 // nodo successivo
};

/*
 * Tabellone
 *  - Rappresenta la classifica di un singolo tema.
 *  - nomeTema punta alla stringa temiQuiz[i].nome (memoria condivisa).
 */
struct Tabellone {
    char*                 nomeTema;     // etichetta del tema
    struct NodoPunteggio* head;         // testa della lista classifica
    pthread_mutex_t       lock;         // mutex per accessi concorrenti
};

/*
 * CoppiaQ
 *  - Una domanda e la sua risposta corretta.
 */
struct CoppiaQ {
    char domanda[MaxReadQuestL];        // contiene già il '?'
    char risposta[MaxReadL];
};

/*
 * TemaQuiz
 *  - Un tema con il suo nome (senza .txt) e l'array di NumQuest domande.
 */
struct TemaQuiz {
    char           nome[MaxReadL];
    struct CoppiaQ quiz[NumQuest];
};

// ==================== Variabili Globali ====================

static int                   sd_ascolto;                // socket di ascolto
static int                   numTemi = 0;               // numero di file .txt in qa/
static struct TemaQuiz*      temiQuiz   = NULL;         // vettore dinamico dei temi
static struct Tabellone*     tabelloni  = NULL;         // classifica per ciascun tema
static struct GiocatoreStato giocatori[MAX_THREAD];     // 1 slot per thread

// Sincronizzazione “stampa stato”
static pthread_mutex_t mtx_score;
static pthread_cond_t  cond_score;
static int             flag_stampa = 0;                 // 0 = nessuna richiesta 
                                                        // 1 = richiesta attiva

// Protezioni varie
static pthread_mutex_t mtx_sd;                  // serialize accept() tra i thread
static pthread_mutex_t mtx_players;             // protezione array giocatori[]

// ---- Shutdown controllato (premi 'Q' + Invio) ----
static atomic_int server_shutdown = 0;          // 0=on 
                                                // 1=shutdown

// Tracciamento connessioni attive per chiusura pulita (spegnimento)
static int            conn_sd_list[MAX_THREAD];             // -1 = libero, altrimenti sd attivo
static pthread_mutex_t mtx_conns;

// ==================== Prototipi ====================
static void* threadConnessione(void* arg);
static void  gestisciConnessione(int conn_sd, struct GiocatoreStato* gioc);

static int   verificaRicezione(int ret, int len);           // helper, robusto su recv()
static void  normalizza(const char* in, char* out, size_t cap);

static void  inviaClassifica(int conn_sd);                  // show-score
static void  inviaPunteggiRicorsivo(struct NodoPunteggio* n, int conn_sd, int* cont);

static int   caricaDomande(const char* percorso, struct CoppiaQ* quiz);
static int   costruisciIndice(void);

static void  stampaSezioneTemi(void);
static void  stampaSezioneOnline(void);
static void  stampaSezioneClassifiche(void);
static void  stampaStato(void);

static void* consoleWatcher(void*);                         // thread che attende 'Q' su stdin

// Rimozione completa del nickname da *tutte* le classifiche
static void  rimuovi_dalle_classifiche(const char* nick);

// ============================================================================
// main
// ============================================================================
int main() {
    struct sockaddr_in addr;
    pthread_t threads[MAX_THREAD];

    // --- 1) Costruzione indice temi -----------------------------------
    if (costruisciIndice() < 0) {
        fprintf(stderr, "[ERR] impossibile costruire indice temi da '%s'\n", QA_FOLDER);
        return -1;
    }

    // --- 2) Caricamento domande da file -------------------------------
    char path[MaxReadQuestL + sizeof(QA_FOLDER)];
    for (int i = 0; i < numTemi; i++) {

        // path "qa/<nome>.txt"
        snprintf(path, sizeof(path), "%s%s.txt", QA_FOLDER, temiQuiz[i].nome);

        if (caricaDomande(path, temiQuiz[i].quiz) < 0) {
            fprintf(stderr, "[ERR] lettura domande fallita: %s\n", path);
            return -1;
        }

    }

    // --- 3) Socket di ascolto -----------------------------------------
    sd_ascolto = socket(AF_INET, SOCK_STREAM, 0);
    if (sd_ascolto < 0) { perror("socket"); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, IPADDR, &addr.sin_addr);

    if (bind(sd_ascolto, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(sd_ascolto, MAX_THREAD + 5) < 0) { perror("listen"); return -1; }

    // Init protezioni
    pthread_mutex_init(&mtx_sd, NULL);
    pthread_mutex_init(&mtx_players, NULL);
    pthread_mutex_init(&mtx_score, NULL);
    pthread_cond_init(&cond_score, NULL);
    pthread_mutex_init(&mtx_conns, NULL);

    // Inizializza slot giocatori e tabella connessioni
    for (int i = 0; i < MAX_THREAD; i++) {
        giocatori[i].nome[0] = '\0';
        giocatori[i].temaCorr = NULL;
        conn_sd_list[i] = -1;
    }
    for (int i = 0; i < numTemi; i++) {
        pthread_mutex_init(&tabelloni[i].lock, NULL);
    }

    // Banner iniziale e prima stampa stato
    printf("--- Server in ascolto su %s:%d ---\n\n", IPADDR, SERVER_PORT);
    fflush(stdout);
    stampaStato();

    // --- 4) Thread console: shutdown 'Q' -------------------------------
    pthread_t t_console;
    pthread_create(&t_console, NULL, consoleWatcher, NULL);

    // --- 5) Avvio worker thread ---------------------------------------
    for (int i = 0; i < MAX_THREAD; i++) {
        int* idx = (int*)malloc(sizeof(int));
        *idx = i;
        pthread_create(&threads[i], NULL, threadConnessione, idx);
    }

    // --- 6) Loop di ristampa stato -----------------------------------
    while (1) {
        // attesa richiesta di refresh (alzata dai worker)
        pthread_mutex_lock(&mtx_score);

        while (!flag_stampa) pthread_cond_wait(&cond_score, &mtx_score);

        flag_stampa = 0;                           // consumo richiesta

        pthread_mutex_unlock(&mtx_score);

        // stampa le tre sezioni richieste
        stampaStato();

        // sblocca il worker che ha richiesto la stampa (handshake)
        pthread_mutex_lock(&mtx_score);
        pthread_cond_signal(&cond_score);
        pthread_mutex_unlock(&mtx_score);
    }
    return 0;
}

// ============================================================================
// threadConnessione
// ============================================================================
static void* threadConnessione(void* arg) {
    int idx = *(int*)arg; 
    free(arg);

    while (1) {
        // accept serializzato (un solo thread alla volta)
        pthread_mutex_lock(&mtx_sd);
        int conn_sd = accept(sd_ascolto, NULL, NULL);
        pthread_mutex_unlock(&mtx_sd);
        if (conn_sd < 0) {
            if (atomic_load(&server_shutdown)) break;
            continue;
        }

        // registra la connessione per chiusura "gentile" allo shutdown
        pthread_mutex_lock(&mtx_conns);
        conn_sd_list[idx] = conn_sd;
        pthread_mutex_unlock(&mtx_conns);

        // esegue protocollo di sessione
        gestisciConnessione(conn_sd, &giocatori[idx]);

        // chiude e deregistra
        close(conn_sd);
        pthread_mutex_lock(&mtx_conns);
        conn_sd_list[idx] = -1;
        pthread_mutex_unlock(&mtx_conns);

        if (atomic_load(&server_shutdown)) break;
    }
    return NULL;
}

// ============================================================================
// normalizza
// ----------------------------------------------------------------------------
// Confronto più tollerante per le risposte.
// - Converte in minuscolo.
// - Ignora TUTTI i whitespace.
// - Ignora caratteri non-ASCII (es. apostrofo tipografico ’, lettere accentate).
// - Ignora punteggiatura e simboli vari: conserva solo [a-z0-9].
// Serve tutto a evitare i mismatch per piccolezze o spazi o punteggiatura.
// ============================================================================
static void normalizza(const char* in, char* out, size_t cap) {
    size_t k = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && k + 1 < cap; ++p) {
        unsigned char c = *p;
        if (isspace(c)) continue;                 // ignora spazi/TAB/CR/LF
        if (c & 0x80) continue;                   // ignora byte non-ASCII
        c = (unsigned char)tolower(c);
        if (isalnum(c)) out[k++] = (char)c;       // tieni solo [a-z0-9]
    }
    out[k] = '\0';
}

// ============================================================================
// gestisciConnessione (protocollo completo)
// ============================================================================
static void gestisciConnessione(int conn_sd, struct GiocatoreStato* gioc) {
    uint16_t netNum;
    int ret;
    char buffer[MaxReadQuestL];
    char nick_attuale[MaxUsernameL] = {0};

    // --- (1) invia numero di temi disponibili -------------------------
    netNum = htons(numTemi);
    send(conn_sd, &netNum, sizeof(netNum), MSG_NOSIGNAL);

    // --- (2) login/validazione nickname -------------------------------
    do {
        // riceve nickname (lunghezza fissa MaxUsernameL come da protocollo)
        ret = recv(conn_sd, buffer, MaxUsernameL, MSG_WAITALL);
        if (verificaRicezione(ret, MaxUsernameL) != 0) goto fine;

        // comandi “fuori sessione quiz”
        if (strcmp(buffer, ShowScore) == 0) { inviaClassifica(conn_sd); continue; }
        if (strcmp(buffer, EndQuiz)   == 0) { goto fine; }

        // controllo univocità tra gli slot attivi (giocatori[]):
        int ok = 1;
        pthread_mutex_lock(&mtx_players);
        for (int i = 0; i < MAX_THREAD; i++) {
            if (&giocatori[i] != gioc &&
                giocatori[i].nome[0] != '\0' &&
                strcmp(giocatori[i].nome, buffer) == 0) {
                ok = 0; 
                break;
            }
        }
        if (ok) {
            strncpy(gioc->nome, buffer, MaxUsernameL);
            strncpy(nick_attuale, buffer, MaxUsernameL);
        }
        pthread_mutex_unlock(&mtx_players);

        // rispondo col verdetto (0/1) in uint16_t rete
        netNum = htons(ok);
        send(conn_sd, &netNum, sizeof(netNum), MSG_NOSIGNAL);
    } while (!ntohs(netNum));

    // segna “online” (temaCorr NULL finché non entra in un quiz)
    pthread_mutex_lock(&mtx_players);
    gioc->temaCorr = NULL;
    pthread_mutex_unlock(&mtx_players);

    // --- (3) invio elenco nomi tema -----------------------------------
    for (int i = 0; i < numTemi; i++) {
        send(conn_sd, temiQuiz[i].nome, MaxReadL, MSG_NOSIGNAL);
    }

    // refresh "Utenti online"
    pthread_mutex_lock(&mtx_score);
    flag_stampa = 1; 
    pthread_cond_signal(&cond_score);
    while (flag_stampa) pthread_cond_wait(&cond_score, &mtx_score);
    pthread_mutex_unlock(&mtx_score);

    // --- (4) ciclo di gioco -------------------------------------------
    while (1) {
        // ricevo comando: 0=end 
        // 1=showscore
        // >=3 selezione tema
        ret = recv(conn_sd, &netNum, sizeof(netNum), MSG_WAITALL);
        if (verificaRicezione(ret, sizeof(netNum)) != 0) goto fine;
        int cmd = ntohs(netNum);

        // Fine sessione: esci dal loop.
        if (cmd == 0) {
            break;
        }
        if (cmd == 1) {
            inviaClassifica(conn_sd);
            continue;
        }

        // selezione tema: protocollo usa offset +2 per i comandi, quindi:
        //   cmd = (idTema + 2) + 1  => idTema = cmd - 3
        int temaIdx = cmd - 3;

        // marca lo stato: “sto svolgendo <tema>”
        pthread_mutex_lock(&mtx_players);
        gioc->temaCorr = temiQuiz[temaIdx].nome;
        pthread_mutex_unlock(&mtx_players);

        // inserisce il giocatore nella classifica del tema (in testa)
        pthread_mutex_lock(&tabelloni[temaIdx].lock);
        struct NodoPunteggio* nodo = (struct NodoPunteggio*)malloc(sizeof(*nodo));
        nodo->punteggio = 0;
        nodo->finito    = 0;                        // ancora non terminato
        strncpy(nodo->nick, gioc->nome, MaxUsernameL);
        nodo->nick[MaxUsernameL-1] = '\0';
        nodo->prev      = NULL;
        nodo->nxt       = tabelloni[temaIdx].head;  // nuova testa
        if (tabelloni[temaIdx].head) tabelloni[temaIdx].head->prev = nodo;
        tabelloni[temaIdx].head = nodo;
        pthread_mutex_unlock(&tabelloni[temaIdx].lock);

        // refresh
        pthread_mutex_lock(&mtx_score);
        flag_stampa = 1; pthread_cond_signal(&cond_score);
        while (flag_stampa) pthread_cond_wait(&cond_score, &mtx_score);
        pthread_mutex_unlock(&mtx_score);

        // loop domande NumQuest
        for (int q = 0; q < NumQuest; q++) {
            // invio testo domanda (buffer a lunghezza fissa MaxReadQuestL)
            send(conn_sd, temiQuiz[temaIdx].quiz[q].domanda, MaxReadQuestL, MSG_NOSIGNAL);

            // ricevo risposta (buffer a lunghezza fissa MaxReadL)
            ret = recv(conn_sd, buffer, MaxReadL, MSG_WAITALL);
            if (verificaRicezione(ret, MaxReadL) != 0) goto fine;

            // comandi inline: show-score / end
            if (strcmp(buffer, ShowScore) == 0) { inviaClassifica(conn_sd); q--; continue; }
            if (strcmp(buffer, EndQuiz)   == 0) { goto fine; }

            // confronto risposta: normalizzazione più robusta (vedi sopra normalizza)
            char attesa[MaxReadL];  char ricevuta[MaxReadL];
            normalizza(temiQuiz[temaIdx].quiz[q].risposta, attesa,   sizeof(attesa));
            normalizza(buffer,                              ricevuta, sizeof(ricevuta));

            int esito = strcmp(ricevuta, attesa);       // 0 = corretta

            if (esito == 0) {
                // +1 punto e “bubble up” nella classifica
                // con tie-break sul tempo quando necessario
                pthread_mutex_lock(&tabelloni[temaIdx].lock);
                nodo->punteggio++;

                while (nodo->nxt &&
                       (nodo->punteggio > nodo->nxt->punteggio ||
                        (nodo->punteggio == nodo->nxt->punteggio &&
                         nodo->finito && nodo->nxt->finito && nodo->finito < nodo->nxt->finito))) {

                    struct NodoPunteggio* prev = nodo->prev;
                    struct NodoPunteggio* next = nodo->nxt;

                    if (prev) prev->nxt = next;
                    else      tabelloni[temaIdx].head = next;
                    next->prev = prev;

                    nodo->nxt  = next->nxt;
                    nodo->prev = next;
                    if (next->nxt) next->nxt->prev = nodo;
                    next->nxt = nodo;
                }
                pthread_mutex_unlock(&tabelloni[temaIdx].lock);
            }

            // refresh sezione Classifiche dopo ogni risposta
            pthread_mutex_lock(&mtx_score);
            flag_stampa = 1; pthread_cond_signal(&cond_score);
            while (flag_stampa) pthread_cond_wait(&cond_score, &mtx_score);
            pthread_mutex_unlock(&mtx_score);

            // invio esito (0 = corretta, 1 = errata)
            netNum = htons(esito);
            send(conn_sd, &netNum, sizeof(netNum), MSG_NOSIGNAL);
        }

        // quiz terminato: timestamp di fine, riordina per tie-break (parità di punteggio)
        pthread_mutex_lock(&tabelloni[temaIdx].lock);
        nodo->finito = time(NULL);                  // istante di fine
        while (nodo->nxt &&
               nodo->punteggio == nodo->nxt->punteggio &&
               nodo->finito && nodo->nxt->finito &&
               nodo->finito < nodo->nxt->finito) {

            struct NodoPunteggio* prev = nodo->prev;
            struct NodoPunteggio* next = nodo->nxt;

            if (prev) prev->nxt = next;
            else      tabelloni[temaIdx].head = next;
            next->prev = prev;

            nodo->nxt  = next->nxt;
            nodo->prev = next;
            if (next->nxt) next->nxt->prev = nodo;
            next->nxt = nodo;
        }
        pthread_mutex_unlock(&tabelloni[temaIdx].lock);

        // esco dal tema corrente
        pthread_mutex_lock(&mtx_players);
        gioc->temaCorr = NULL;
        pthread_mutex_unlock(&mtx_players);

        // refresh stato finale dopo il tema
        pthread_mutex_lock(&mtx_score);
        flag_stampa = 1; pthread_cond_signal(&cond_score);
        while (flag_stampa) pthread_cond_wait(&cond_score, &mtx_score);
        pthread_mutex_unlock(&mtx_score);
    }

fine:
    // Cleanup finale: libero slot online e cancello il nick da tutte le classifiche.
    pthread_mutex_lock(&mtx_players);
    gioc->nome[0]  = '\0';
    gioc->temaCorr = NULL;
    pthread_mutex_unlock(&mtx_players);

    rimuovi_dalle_classifiche(nick_attuale);

    // refresh schermo
    pthread_mutex_lock(&mtx_score);
    flag_stampa = 1; pthread_cond_signal(&cond_score);
    while (flag_stampa) pthread_cond_wait(&cond_score, &mtx_score);
    pthread_mutex_unlock(&mtx_score);
}

// ============================================================================
// verificaRicezione
// ============================================================================
static int verificaRicezione(int ret, int len) {
    if (ret == 0)             return 1;         // peer chiuso
    if (ret < 0 || ret < len) return -1;        // errore/parziale
    return 0;                                   // OK
}

// ============================================================================
// inviaClassifica / inviaPunteggiRicorsivo
// ============================================================================
static void inviaPunteggiRicorsivo(struct NodoPunteggio* n, int conn_sd, int* cont) {
    if (!n) {
        uint16_t net = htons(*cont);
        send(conn_sd, &net, sizeof(net), MSG_NOSIGNAL);         // invio numero giocatori
        return;
    }
    (*cont)++;
    inviaPunteggiRicorsivo(n->nxt, conn_sd, cont);              // invio dal peggiore al migliore
    send(conn_sd, n->nick, MaxReadL, MSG_NOSIGNAL);
    uint16_t net = htons(n->punteggio);
    send(conn_sd, &net, sizeof(net), MSG_NOSIGNAL);
}

static void inviaClassifica(int conn_sd) {
    for (int i = 0; i < numTemi; i++) {
        send(conn_sd, tabelloni[i].nomeTema, MaxReadL, MSG_NOSIGNAL);
        int cont = 0;
        pthread_mutex_lock(&tabelloni[i].lock);
        inviaPunteggiRicorsivo(tabelloni[i].head, conn_sd, &cont);
        pthread_mutex_unlock(&tabelloni[i].lock);
    }
}

// ============================================================================
// rimuovi_dalle_classifiche
// ============================================================================
static void rimuovi_dalle_classifiche(const char* nick) {
    if (!nick || !nick[0]) return;
    for (int t = 0; t < numTemi; ++t) {
        pthread_mutex_lock(&tabelloni[t].lock);
        struct NodoPunteggio* n = tabelloni[t].head;
        while (n) {
            struct NodoPunteggio* next = n->nxt;
            if (strcmp(n->nick, nick) == 0) {
                if (n->prev) n->prev->nxt = n->nxt; else tabelloni[t].head = n->nxt;
                if (n->nxt)  n->nxt->prev = n->prev;
                free(n);
            }
            n = next;
        }
        pthread_mutex_unlock(&tabelloni[t].lock);
    }
}

// ============================================================================
// I/O file: costruisciIndice / caricaDomande
// ----------------------------------------------------------------------------
// Scansione 'qa/' per contare i file .txt, allocare gli array e popolare i nomi.
// Lettura domande a coppie di righe:
//    riga dispari  -> Domanda (terminante in '?')
//    riga pari     -> Risposta
// CR/LF safe, trim di coda/spazi e tolleranza a linee vuote accidentali.
// ============================================================================
static int costruisciIndice(void) {
    DIR* dir = opendir(QA_FOLDER);
    if (!dir) return -1;
    struct dirent* ent;

    // 1) conteggio file .txt
    while ((ent = readdir(dir))) {
        if (strstr(ent->d_name, ".txt")) numTemi++;
    }
    closedir(dir);
    if (numTemi <= 0) return -1;

    // 2) allocazioni
    temiQuiz  = (struct TemaQuiz*)  malloc(numTemi * sizeof(*temiQuiz));
    tabelloni = (struct Tabellone*) malloc(numTemi * sizeof(*tabelloni));
    if (!temiQuiz || !tabelloni) return -1;

    // 3) popolamento nomi tema e init tabelloni
    dir = opendir(QA_FOLDER);
    int idx = 0;
    while ((ent = readdir(dir))) {
        char* pos;
        if ((pos = strstr(ent->d_name, ".txt"))) {
            *pos = '\0';                                        // rimuovo estensione
            strncpy(temiQuiz[idx].nome, ent->d_name, MaxReadL);
            temiQuiz[idx].nome[MaxReadL-1] = '\0';
            tabelloni[idx].nomeTema = temiQuiz[idx].nome;       // puntatore condiviso
            tabelloni[idx].head     = NULL;
            idx++;
        }
    }
    closedir(dir);
    return 0;
}

// helper trim (CR, spazi, TAB, LF)
static void trim_line(char* s){
    if (!s) return;
    s[strcspn(s, "\n")] = '\0';                                  // taglia \n
    size_t L = strlen(s);
    while (L>0 && (s[L-1]=='\r'||s[L-1]==' '||s[L-1]=='\t')) s[--L]='\0';
}

static int caricaDomande(const char* percorso, struct CoppiaQ* quiz) {
    FILE* f = fopen(percorso, "r");
    if (!f) return -1;

    char domanda[MaxReadQuestL + MaxReadL];
    char risposta[MaxReadL + 32];

    for (int i = 0; i < NumQuest; i++) {
        // Leggi DOMANDA (salta eventuali righe vuote)
        do {
            if (!fgets(domanda, sizeof(domanda), f)) { fclose(f); return -1; }
            trim_line(domanda);
        } while (domanda[0] == '\0');

        // Se manca il '?' finale non è un problema: la domanda viene usata “as is”
        // (ma UI domande lo prevede nei file).

        // Leggi RISPOSTA (salta eventuali righe vuote)
        do {
            if (!fgets(risposta, sizeof(risposta), f)) { fclose(f); return -1; }
            trim_line(risposta);
        } while (risposta[0] == '\0');

        // Copie protette nelle strutture
        strncpy(quiz[i].domanda,  domanda,  MaxReadQuestL);
        quiz[i].domanda[MaxReadQuestL-1] = '\0';
        strncpy(quiz[i].risposta, risposta, MaxReadL);
        quiz[i].risposta[MaxReadL-1] = '\0';
    }

    fclose(f);
    return 0;
}

// ============================================================================
// Stampa Stato: tre sezioni + prompt shutdown
// ============================================================================
static void stampaSezioneTemi(void) {
    printf("== Test disponibili (%d) ==\n", numTemi);
    for (int i = 0; i < numTemi; i++) {
        printf("  %2d) %s\n", i + 1, temiQuiz[i].nome);
    }
    StampaNumPiu();
}

static void stampaSezioneOnline(void) {
    int online = 0;
    for (int i = 0; i < MAX_THREAD; i++) if (giocatori[i].nome[0] != '\0') online++;

    printf("== Utenti online (%d) ==\n", online);
    for (int i = 0; i < MAX_THREAD; i++) {
        if (giocatori[i].nome[0] == '\0') continue;

        printf("- %s", giocatori[i].nome);
        if (giocatori[i].temaCorr) printf("  [sta facendo: %s]\n", giocatori[i].temaCorr);
        else                       printf("\n");

        // Per ogni tema, se trovo un nodo di classifica di questo utente, lo mostro:
        for (int t = 0; t < numTemi; t++) {
            pthread_mutex_lock(&tabelloni[t].lock);
            struct NodoPunteggio* n = tabelloni[t].head;
            while (n) {
                if (strcmp(n->nick, giocatori[i].nome) == 0) {
                    printf("    • %s  -> %u/%d%s\n",
                           tabelloni[t].nomeTema, n->punteggio, NumQuest,
                           (n->finito ? "" : " (in corso)"));
                    break;                      // trovato un record per questo tema
                }
                n = n->nxt;
            }
            pthread_mutex_unlock(&tabelloni[t].lock);
        }
    }
    StampaNumPiu();
}

static void stampaSezioneClassifiche(void) {
    printf("== Classifiche per test ==\n");
    for (int t = 0; t < numTemi; t++) {
        printf("[%s]\n", tabelloni[t].nomeTema);
        pthread_mutex_lock(&tabelloni[t].lock);

        struct NodoPunteggio* n = tabelloni[t].head;
        int pos = 1;
        while (n) {
            if (n->finito) {
                // formatta orario locale “Ora:Minuto:Secondo”
                struct tm tmv;
                char when[32];
                localtime_r(&n->finito, &tmv);
                strftime(when, sizeof(when), "%H:%M:%S", &tmv);
                printf("  %2d) %-16s  %u/%d  (finito: %s)\n",
                       pos, n->nick, n->punteggio, NumQuest, when);
            } else {
                printf("  %2d) %-16s  %u/%d  (in corso)\n",
                       pos, n->nick, n->punteggio, NumQuest);
            }
            n = n->nxt; pos++;
        }

        pthread_mutex_unlock(&tabelloni[t].lock);
        printf("\n");
    }
    StampaNumPiu();
}

static void stampaStato(void) {
    system("clear");
    printf("Trivia Quiz – Stato Server\n");
    StampaNumPiu();

    stampaSezioneTemi();
    stampaSezioneOnline();
    stampaSezioneClassifiche();

    // Prompt per spegnimento controllato
    printf("\nShut down del server: premi 'Q' e INVIO\n");
    fflush(stdout);
}

// ============================================================================
// consoleWatcher
// ============================================================================
static void* consoleWatcher(void* _) {
    (void)_;
    int ch;
    while ((ch = getchar()) != EOF) {
        if (ch == 'q' || ch == 'Q') {
            atomic_store(&server_shutdown, 1);

            // chiude listening: interrompe gli accept pending
            shutdown(sd_ascolto, SHUT_RDWR);
            close(sd_ascolto);

            // chiude "gentilmente" tutte le connessioni attive
            pthread_mutex_lock(&mtx_conns);
            for (int i = 0; i < MAX_THREAD; i++) {
                if (conn_sd_list[i] >= 0) {
                    shutdown(conn_sd_list[i], SHUT_RDWR);
                    close(conn_sd_list[i]);
                    conn_sd_list[i] = -1;
                }
            }
            pthread_mutex_unlock(&mtx_conns);

            printf("\n[Server] Shutdown richiesto. Sto terminando...\n\n");
            fflush(stdout);

            // Piccola attesa per permettere ai client di ricevere EOF
            usleep(200 * 1000);                              // 200 ms
            _exit(0);
        }
    }
    return NULL;
}
