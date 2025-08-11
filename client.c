// ============================================================================
// Autore: de Dato A. – Matricola: 635256
// CLIENT – Trivia Quiz
//
// Funzioni chiave:
//  - Menu principale con nickname preservato tra sessioni (Invio = conferma)
//  - Selezione temi dei quiz con numero corrispondente (n)
//  - Comando "Mostra Punteggio", classifica player OnLine
//  - Quiz a domande con input robusto e invio in buffer azzerato
//  - Rilevamento immediato shutdown server (select su stdin+socket)
//  - Persistenza in-memoria (per nickname) dei temi già svolti anche se torni al menu
//
// ============================================================================

#include "utility.h"
#include <sys/select.h>   // select() per gestione I/O reattiva
#include <errno.h>        // errno per select
#include <ctype.h>        // isspace (per trim locale)

// ---------------------------- Colorazione ANSI --------------------------------
#define COL_OK    "\x1b[32m"
#define COL_ERR   "\x1b[31m"
#define COL_DIM   "\x1b[2m"
#define COL_BOLD  "\x1b[1m"
#define COL_RST   "\x1b[0m"

// ---------------------------- Protocollo (client->server) ---------------------
// NB: lato server l’handler decodifica così:
//  0 = end session, 
//  1 = show score, 
//  >=3 tema scelto (temaIdx = cmd - 3)
#define CMD_END         0
#define CMD_SHOW        1
#define CMD_THEME_BASE  3   // manda: CMD_THEME_BASE + (idTema - 1)

// ---------------------------- Strutture dati client ---------------------------

// Tema disponibile (lista semplice)
struct Tema {
    char          nome[MaxReadL];  // etichetta (“cinema”, “informatica”...)
    int           id;              // 1..N (ordine di ricezione)
    struct Tema*  next;
};

// Riepilogo quiz svolti NELLA SESSIONE CORRENTE (solo lato client, locale)
struct Completato {
    char               nomeTema[MaxReadL];
    unsigned int       punti;      // risposte corrette su NumQuest
    struct Completato* next;
};

// Persistenza minima per nickname: bitmask dei temi già completati
// (resta valida finché non chiudi il programma client)
struct Profilo {
    char          nick[MaxUsernameL];
    unsigned int  mask_done;       // bit i-esimo = tema i (1==completato)
    struct Profilo* next;
};

// ---------------------------- Stato client globale ----------------------------
static int  g_server_spento = 0;            // quando 1: consenti solo “2) Esci”
static char g_last_nick[MaxUsernameL] = ""; // nickname riutilizzato (Invio per confermare)

// archivio profili in-memoria
static struct Profilo* g_profili = NULL;

// ---------------------------- Utility UI --------------------------------------
static inline void riga() { StampaNumPiu(); }
static void titolo(const char* s)   { printf(COL_BOLD "%s" COL_RST "\n", s); }
static void nota_dim(const char* s) { printf(COL_DIM  "%s" COL_RST "\n", s); }

// Trim robusto: toglie spazi/TAB e CR/LF a sinistra e destra
static void trim(char* s){
    if(!s) return;

    // left trim
    size_t i = 0; while (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n') i++;
    if (i) memmove(s, s+i, strlen(s+i)+1);

    // right trim
    size_t n = strlen(s);
    while (n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = '\0';

}

// ---------------------------- Lettura input (2 varianti) ----------------------
//
// 1) leggiLinea(): uso semplice (solo stdin), rifiuta vuote
// 2) leggiLinea_reactive(): I/O reattivo con select() su stdin + socket
//    - se il server chiude mentre attendo input, appare subito il messaggio
//    - se default_nick != NULL, Invio vuoto lo accetta (Utile per il login)
// -----------------------------------------------------------------------------

static int leggiLinea(const char* prompt, char* buf, int maxLen){
    while (1) {
        printf("%s: ", prompt); fflush(stdout);
        if (!fgets(buf, maxLen, stdin)) return 0;
        buf[strcspn(buf, "\n")] = '\0';
        trim(buf);

        if ((int)strlen(buf) >= maxLen-1) {
            int c; while ((c=getchar())!='\n' && c!=EOF);
            printf("Input troppo lungo. Riprova.\n");
            continue;
        }
        if (strlen(buf)==0) {
            printf("Inserire almeno un carattere.\n");
            continue;
        }
        return 1;
    }
}

//  Ritorni:  1 ok, 
//  0 EOF stdin, 
//  -2 server spento (setta g_server_spento)
static int leggiLinea_reactive(int sd, const char* prompt, char* buf, int maxLen,
                               const char* default_nick /*accettato con Invio|NULL no default*/)
{
    for (;;) {
        if (g_server_spento) return -2;

        if (default_nick && default_nick[0])
            printf("%s [" COL_BOLD "%s" COL_RST "]: ", prompt, default_nick);
        else
            printf("%s: ", prompt);
        fflush(stdout);

        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        if (sd >= 0) FD_SET(sd, &rfds);
        int nfds = (sd>STDIN_FILENO ? sd : STDIN_FILENO) + 1;

        int sel = select(nfds, &rfds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            return 0;
        }

        // socket leggibile? -> verifica se è shutdown (EOF)
        if (sd >= 0 && FD_ISSET(sd, &rfds)) {
            char peek;
            int r = recv(sd, &peek, 1, MSG_PEEK);
            if (r <= 0) {
                g_server_spento = 1;
                printf("\n" COL_ERR "Il server è stato spento." COL_RST "\n");
                printf("Premi " COL_BOLD "2" COL_RST " per uscire.\n\n");
                return -2;
            }
        }

        // stdin leggibile
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (!fgets(buf, maxLen, stdin)) return 0;
            buf[strcspn(buf, "\n")] = '\0';
            trim(buf);

            if ((int)strlen(buf) >= maxLen-1) {
                int c; while ((c=getchar())!='\n' && c!=EOF);
                printf("Input troppo lungo. Riprova.\n");
                continue;
            }
            if (strlen(buf)==0 && default_nick && default_nick[0]) {
                // Invio a vuoto -> conferma default
                strncpy(buf, default_nick, maxLen);
                return 1;
            }
            if (strlen(buf)==0) {
                printf("Inserire almeno un carattere.\n");
                continue;
            }
            return 1;
        }
    }
}

// ---------------------------- Liste utilitarie --------------------------------
static void liberaTemi(struct Tema* p){ while(p){ struct Tema* t=p; p=p->next; free(t);} }
static struct Tema* estraiTema(struct Tema** head, int id){
    struct Tema *pr=NULL,*cu=*head;
    while (cu) {
        if (cu->id == id) {
            if (pr) pr->next = cu->next; else *head = cu->next;
            return cu;
        }
        pr = cu; cu = cu->next;
    }
    return NULL;
}
static void aggiungiCompletato(struct Completato** s, const char* nome, unsigned int punti){
    struct Completato* n = (struct Completato*)malloc(sizeof(*n));
    strncpy(n->nomeTema, nome, MaxReadL); n->nomeTema[MaxReadL-1] = '\0';
    n->punti = punti; 
    n->next = *s; 
    *s = n;
}
static void liberaCompletati(struct Completato* s){ while(s){ struct Completato* t=s; s=s->next; free(t);} }

// profilo per nickname (crea se non esiste)
static struct Profilo* get_profilo(const char* nick){
    for (struct Profilo* p=g_profili; p; p=p->next)
        if (strncmp(p->nick, nick, MaxUsernameL)==0) return p;
    struct Profilo* n = (struct Profilo*)calloc(1,sizeof(*n));
    strncpy(n->nick, nick, MaxUsernameL); n->nick[MaxUsernameL-1]='\0';
    n->next = g_profili; 
    g_profili = n;
    return n;
}

// ---------------------------- UI ad alto livello ------------------------------
static void menuPrincipale(){
    printf("\n");
    titolo("Il Gioco di Trivia Quiz!");
    riga();
    printf("1) Avvia sessione di quiz\n");
    printf("2) Esci\n");
    riga();
}

static void stampaTemi(struct Tema* l){
    titolo("Temi disponibili");
    riga();
    for (struct Tema* t=l; t; t=t->next) printf("%d) %s\n", t->id, t->nome);
    riga();
    nota_dim("\n Suggerimenti:");
    nota_dim(" - Digita " COL_BOLD "Mostra Punteggio" COL_RST COL_DIM
         " per la Classifica Globale dei Giocatori On Line.");
    nota_dim(" - Digita " COL_BOLD "0" COL_RST COL_DIM
         " per tornare al menu principale.");
    nota_dim("(Nota: se torni al menù principale, non potrai comunque rifare i quiz già sostenuti su questo profilo, e sarai rimosso dalla Classifica Globale.)\n");

}

static void stampaCompletati(struct Completato* s){
    titolo("Quiz svolti (sessione corrente)");
    if (!s) printf(COL_DIM "— nessuno —" COL_RST "\n" "\n");
    else for (; s; s=s->next) printf(" • %-16s  %u/%d\n\n", s->nomeTema, s->punti, NumQuest);
    riga();
}

// ---------------------------- Messaggistica shutdown --------------------------
static void serverSpento_print(){
    if (!g_server_spento) return;
    printf(COL_ERR "Il server è stato spento." COL_RST "\n");
    printf("Premi " COL_BOLD "2" COL_RST " per uscire.\n\n");
}

// ---------------------------- Show-score (classifiche online) -----------------
// Riceve dal server (per ogni tema): nome tema, numero record, coppie (nick, punteggio).
// Formatta in modo vicino a quello del server.
static int riceviClassifiche(int sd, int nTemi, char* buf){
    uint16_t net; int ret, n;

    titolo("Classifica Online");
    riga();
    for (int i=0; i<nTemi; i++) {
        // nome tema
        ret = recv(sd, buf, MaxReadL, MSG_WAITALL);
        ret = RecErr(ret, MaxReadL);
        if (ret){ if (ret<0) perror("recv tema"); serverSpento_print(); return -1; }
        printf("[%s]\n", buf);

        // numero giocatori
        ret = recv(sd, &net, sizeof(net), MSG_WAITALL);
        ret = RecErr(ret, sizeof(net));
        if (ret){ if (ret<0) perror("recv num"); serverSpento_print(); return -1; }
        n = ntohs(net);

        // ogni record
        for (int j=0; j<n; j++) {
            ret = recv(sd, buf, MaxReadL, MSG_WAITALL);
            ret = RecErr(ret, MaxReadL);
            if (ret){ if (ret<0) perror("recv nick"); serverSpento_print(); return -1; }
            printf("  - %-16s : ", buf);

            ret = recv(sd, &net, sizeof(net), MSG_WAITALL);
            ret = RecErr(ret, sizeof(net));
            if (ret){ if (ret<0) perror("recv score"); serverSpento_print(); return -1; }
            printf("%d\n", ntohs(net));
        }
        printf("\n");
    }
    riga();
    return 0;
}

// ---------------------------- Sessione quiz (protocollo completo) ------------
static int sessioneQuiz(int sd){
    uint16_t net; int ret;

    // (1) Numero temi
    ret = recv(sd, &net, sizeof(net), MSG_WAITALL);
    ret = RecErr(ret, sizeof(net));
    if (ret){ if (ret<0) perror("recv numTemi"); return 1; }
    int nTemi = ntohs(net);

    // (2) Login nickname (riusa default con Invio vuoto)
    struct Profilo* prof = NULL;
    while (1) {
        char nick[MaxUsernameL];
        const char* def = (g_last_nick[0] ? g_last_nick : NULL);
        if (def) printf("\n"); else printf("\nInserisci il tuo nickname (max %d):\n", MaxUsernameL);

        int lr = leggiLinea_reactive(sd, "Nickname", nick, MaxUsernameL, def);
        if (lr == -2) return 1;                 // server spento
        if (lr == 0)  return -1;                // EOF stdin

        // invia nome in buffer a lunghezza fissa
        char tosend[MaxUsernameL] = {0};
        strncpy(tosend, nick, MaxUsernameL-1);
        if (send(sd, tosend, MaxUsernameL, MSG_NOSIGNAL) < 0) { perror("send nick"); return 1; }

        // comandi speciali prima del login accettato
        if (!strcmp(nick, EndQuiz)) return 0;
        if (!strcmp(nick, ShowScore)){
            if (riceviClassifiche(sd, nTemi, tosend) < 0) return 1;
            continue;
        }

        // ack univocità
        ret = recv(sd, &net, sizeof(net), MSG_WAITALL);
        ret = RecErr(ret, sizeof(net));
        if (ret){ if (ret<0) perror("recv ack"); return 1; }
        int ok = ntohs(net);
        if (ok) { strncpy(g_last_nick, nick, MaxUsernameL); prof = get_profilo(g_last_nick); break; }
        printf(COL_ERR "Nickname già in uso. Riprova." COL_RST "\n");
    }

    // (3) Elenco temi ricevuti dal server — filtriamo localmente quelli già svolti dal profilo
    struct Tema *temi = NULL, *last = NULL;
    char buf[MaxReadL];
    for (int i=1; i<=nTemi; i++) {
        ret = recv(sd, buf, MaxReadL, MSG_WAITALL);
        ret = RecErr(ret, MaxReadL);
        if (ret){ if (ret<0) perror("recv tema"); serverSpento_print(); liberaTemi(temi); return 1; }

        // Se già completato per questo nick
        // NON si vede più tra i selezionabili
        if (prof && (prof->mask_done & (1u << (i-1)))) continue;

        struct Tema* n = (struct Tema*)malloc(sizeof(*n));
        strncpy(n->nome, buf, MaxReadL); n->nome[MaxReadL-1] = '\0';
        n->id = i; n->next = NULL;
        if (!temi) temi = n; else last->next = n; last = n;
    }

    // (4) Storico locale dei quiz svolti (solo per singola sessione)
    struct Completato* stor = NULL;

    // (5) Scelta dei temi + quiz
    while (1) {
        printf("\n");
        stampaTemi(temi);
        stampaCompletati(stor);

        char scelta[MaxReadL];
        int lr = leggiLinea_reactive(sd, "Seleziona", scelta, MaxReadL, NULL);
        if (lr == -2) { liberaTemi(temi); liberaCompletati(stor); return 1; } // server spento
        if (!strcmp(scelta, "0")) {
            uint16_t cmd = htons(CMD_END);
            send(sd, &cmd, sizeof(cmd), MSG_NOSIGNAL); // fine sessione lato server
            liberaTemi(temi); liberaCompletati(stor);
            return 0;  // torna al menu SENZA perdere nickname/profilo
        }
        if (!strcmp(scelta, ShowScore)) {
            uint16_t cmd = htons(CMD_SHOW);
            if (send(sd, &cmd, sizeof(cmd), MSG_NOSIGNAL) < 0) perror("send show");
            if (riceviClassifiche(sd, nTemi, buf) < 0) { liberaTemi(temi); liberaCompletati(stor); return 1; }
            continue;
        }

        int id = atoi(scelta);
        if (id < 1 || id > nTemi) { printf("Scelta non valida.\n"); continue; }

        struct Tema* t = estraiTema(&temi, id);
        if (!t) { printf("Scelta non valida.\n"); continue; }

        // invio selezione tema col mapping corretto (id 1->3, 2->4, ...)
        uint16_t cmd = htons(CMD_THEME_BASE + (id - 1));
        if (send(sd, &cmd, sizeof(cmd), MSG_NOSIGNAL) < 0) {
            perror("send tema"); free(t); liberaTemi(temi); liberaCompletati(stor); return 1;
        }

        printf("\n" COL_BOLD "Tema selezionato: %s" COL_RST "\n", t->nome);

        // --- ciclo domande ---
        unsigned int corrette = 0;
        for (int q=0; q<NumQuest; q++) {
            char domanda[MaxReadQuestL];

            // ricevo domanda
            ret = recv(sd, domanda, MaxReadQuestL, MSG_WAITALL);
            ret = RecErr(ret, MaxReadQuestL);
            if (ret){ if (ret<0) perror("recv domanda"); serverSpento_print(); free(t); liberaTemi(temi); liberaCompletati(stor); return 1; }

            riga();
            printf("Domanda %d: %s\n", q+1, domanda);

            // leggo risposta reattivamente (possibile shutdown mentre scrivo)
            char risposta[MaxReadL];
            int lr2 = leggiLinea_reactive(sd, "Risposta", risposta, MaxReadL, NULL);
            if (lr2 == -2) { free(t); liberaTemi(temi); liberaCompletati(stor); return 1; }

            // comandi inline
            if (!strcmp(risposta, ShowScore)) {
                if (riceviClassifiche(sd, nTemi, buf) < 0) { free(t); liberaTemi(temi); liberaCompletati(stor); return 1; }
                q--; // ripeti stessa domanda
                continue;
            }
            if (!strcmp(risposta, EndQuiz)) {
                // Torna al menu: il server chiude la sessione (é indicato come risposta)
                free(t); liberaTemi(temi); liberaCompletati(stor);
                return 0;
            }

            // invio risposta in buffer AZZERATO (evita scorie)
            char tosend[MaxReadL] = {0};
            strncpy(tosend, risposta, MaxReadL-1);
            if (send(sd, tosend, MaxReadL, MSG_NOSIGNAL) < 0) {
                perror("send risposta"); free(t); liberaTemi(temi); liberaCompletati(stor); return 1;
            }

            // ricevo esito (0=corretta, 1=errata)
            ret = recv(sd, &net, sizeof(net), MSG_WAITALL);
            ret = RecErr(ret, sizeof(net));
            if (ret){ if (ret<0) perror("recv esito"); serverSpento_print(); free(t); liberaTemi(temi); liberaCompletati(stor); return 1; }
            int esito = ntohs(net);

            printf("Esito: ");
            if (esito == 0) { printf(COL_OK  "CORRETTA" COL_RST "\n"); corrette++; }
            else            { printf(COL_ERR "ERRATA"   COL_RST "\n"); }
        }

        // quiz finito: metto storico locale, segno completato nel profilo e mostro sommario
        if (prof) prof->mask_done |= (1u << (t->id - 1));
        aggiungiCompletato(&stor, t->nome, corrette);
        printf("\nHai concluso '%s' con punteggio: " COL_BOLD "%u/%d" COL_RST "\n",
               t->nome, corrette, NumQuest);
        free(t);
    }
}

// ---------------------------- main --------------------------------------------
int main(int argc, char* argv[]){
    if (argc != 2) { printf("Uso: %s <porta>\n", argv[0]); return -1; }
    int porta = atoi(argv[1]);
    if (porta != 4242) {
        printf("Porta non valida (Usa 4242)\n"); return -1;
    }

    struct sockaddr_in srv; memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET; srv.sin_port = htons(porta);
    inet_pton(AF_INET, IPADDR, &srv.sin_addr);

    // Loop menu principale
    while (1) {
        menuPrincipale();

        // Se il server è spento, consenti solo “2) Esci”
        char sc[8];
        if (g_server_spento) {
            nota_dim("\n(Il server è offline: puoi solo uscire.)\n");
            printf("2) Esci\n\n"); 
            riga();
            if (!leggiLinea("Scelta", sc, sizeof(sc))) return -1;
            if (atoi(sc) == 2) { printf("\nTorna presto a Giocare!\n\n"); return 0; }
            continue;
        }

        if (!leggiLinea("Scelta", sc, sizeof(sc))) return -1;
        int op = atoi(sc);
        if (op == 2) { printf("\nTorna presto a Giocare!\n\n"); return 0; }
        if (op != 1)  continue;

        int sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd < 0) { perror("socket"); return -1; }
        if (connect(sd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
            perror("connect"); close(sd); continue;
        }

        int es = sessioneQuiz(sd);
        close(sd);
        if (es < 0) { printf("Errore critico.\n"); return -1; }
    }
}
