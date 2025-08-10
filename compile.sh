# de Dato A. ; Matricola: 635256

clear
gcc -g -Wall -pthread -o server server.c 
gcc -g -Wall -pthread -o client client.c

# ./compile.sh -> fare la roba contenuta in questo file

# ./server -> per avviare il server
# ./client seguito dal numero di porta -> per avviare i client