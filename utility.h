// de Dato A.

#pragma once //Evito di includere più volte il file

// Librerie Standard per I/O, Memoria, Stringhe:
#include <stdio.h>      // Gestione di Input e Output
#include <stdlib.h>     // Allocazione Memoria, Conversione stringhe, Gestione uscita, ecc.
#include <string.h>     // Manipolazione Stringhe e Blocchi di memoria
#include <stdint.h>     // Header libreria standard C99

// Funzioni di Sistema e Gestione dei Segnali:
#include <unistd.h>     // Funzioni POSIX (Read,Write,Close,Sleep,Fork,ecc.)
#include <signal.h>     // Funzioni Gestione Segnali Sistema (Signal,Raise,Kill,ecc.)

// Programmazione di Rete:
#include <sys/types.h>  // Definizione dei tipi usati per chiamate di sistema
#include <sys/socket.h> // Funzioni e Costanti per i SOCKET
#include <arpa/inet.h>  // Funzioni per Conversione indirizzi IP e per Protocolli Internet


// COSTANTI per la configurazione del gioco
#define MaxUsernameL 16
#define MaxReadL 32
#define MaxReadQuestL 96
#define NumPiu 32
#define NumQuest 5


// Messaggi
#define EndQuiz "Fine Quiz"
#define ShowScore "Mostra Punteggio"

// Indirizzo
#define IPADDR "127.0.0.1"



// Funzione per stampare i + della schermata di gioco
void StampaNumPiu(){
    for(size_t i=0; i<NumPiu; i++){
        printf("+");
    }
    printf("\n");
}

// Gestione degli errori posta chiamata nei Socket
int RecErr(int ret, int len){
    if(!ret){
        return 1;
    }else{
        if(ret<0 || ret<len){
            return -1;
        }
    }
    return 0;
}