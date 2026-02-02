# Autobus Podmiejski
## Informacje techniczne
Program stanowi projekt na zaliczenie przedmiotu Systemy Operacyjne. Celem projektu jest zaimplementowanie bezpiecznego i zoptymalizowanego systemu komunikacji wieloprocesowej w środowisku Linux z wykorzystaniem mechanizmów IPC.
- **System operacyjny**: Dowolny system UNIX/LINUX z obsługą System V IPC
- **Wersja kompilatora**: gcc
- **Wymagane biblioteki**: standardowe biblioteki C (pthread, sys/ipc, sys/shm, sys/sem, sys/msg)

## Uruchomienie
```console
$ mkdir build
$ cd build
$ cmake ..
$ make
$ ./main
```
## Mozliwe opcje uruchomienia
```
$ ./main --log=verbose      # Pełne logowanie (domyślnie)
$ ./main --summary          # Logowanie tylko ważnych wydarzeń
$ ./main --quiet            # Logowanie tylko błędów
$ ./main --perf             Tryb wydajnościowy (bez opóźnień symulacyjnych)
$ ./main --full             # Autobusy odjeżdżają gdy są pełne
```

## Założenia projektowe kodu
Program symuluje podmiejską stację autobusową z N autobusami, K kasami biletowymi i ciągle przybywającymi pasażerami
Do tworzenia procesów użyto funkcji fork() i exec() - każdy proces (dyspozytor, kierowca, kasa, pasażer) działa niezależnie

Komunikacja międzyprocesowa realizowana przez mechanizmy IPC System V:

- Pamięć współdzielona dla stanu systemu

- Semafor do synchronizacji dostępu do pamięci współdzielonej

- Kolejki komunikatów dla żądań biletów i wejścia do autobusu

Dodatkowo: 
- System unika "busy-waiting" poprzez blokujące operacje na kolejkach komunikatów i semaforach
- Obsługa sygnałów dla kontroli symulacji (wczesny odjazd, zamknięcie stacji)
- Rozszerzalna konfiguracja przez plik config.h

## Ogólny opis kodu

### Main
	Proces główny (main.c)

	Proces nadrzędny, odpowiedzialny za uruchomienie wszystkich innych procesów

	Tworzy procesy potomne za pomocą fork() i exec():

		- Dyspozytor (dispatcher)

		- Kasy biletowe (ticket_office)

		- Kierowcy (driver)

		- Pasażerowie (passenger) - ciągle tworzeni aż do zamknięcia stacji

	Zarządza opcjami wiersza poleceń i zmiennymi środowiskowymi

	Monitoruje postęp symulacji i inicjuje płynne zakończenie

	Czyści zasoby IPC po zakończeniu


------------------------------------------------------------------

### Dyspozytor
Dyspozytor (dispatcher.c)

	Proces koordynujący symulację

	Inicjalizuje pamięć współdzieloną i struktury IPC

	Obsługuje sygnały kontrolne:

		- SIGUSR1 - wczesny odjazd autobusów

		- SIGUSR2 - zamknięcie stacji (koniec symulacji)

		- SIGINT/SIGTERM - płynne zakończenie

	Monitoruje stan symulacji (liczbę pasażerów, autobusy, bilety)

	Działa jako "nadzorca"/"overseer" - wymusza odjazd autobusów jeśli przekroczyły czas oczekiwania

	Generuje końcowe statystyki

------------------------------------------------------------------

### Kierowca (driver.c)

	Proces zarządzający pojedynczym autobusem

	Odbiera żądania wejścia od pasażerów (kolejka komunikatów)

	Weryfikuje bilety i dostępność miejsc (pasażerskie i na rowery)

	Implementuje dwa wejścia (pasażer/rower) za pomocą oddzielnych semaforów

	Przestrzega harmonogramu odjazdów (co BOARDING_INTERVAL sekund)

	Obsługuje wczesny odjazd na sygnał SIGUSR1 od dyspozytora

	Po odjeździe symuluje czas podróży i powrót na stację

------------------------------------------------------------------

### Kasa biletowa (ticket_office.c)

	Proces sprzedający bilety pasażerom (z wyjątkiem VIP)

	Odbiera żądania biletów z kolejki komunikatów

	Symuluje czas obsługi (TICKET_PROCESS_TIME sekund)

	Aktualizuje statystyki sprzedanych biletów

	Może istnieć wiele kas (TICKET_OFFICES)

------------------------------------------------------------------

### Pasażer (passenger.c)
    Proces reprezentujący pasażera (tylko dorośli)

    Losowo generowane atrybuty: wiek, VIP, rower, dziecko towarzyszące

    Jeśli ma dziecko - tworzy wątek (pthread) reprezentujący dziecko

    Dla nie-VIP: kupuje bilet w kasie

    Czeka na wejście do stacji (limit przez semafor)

    Wysyła żądanie wejścia do autobusu (VIP mają priorytet)

    Czeka na odpowiedź od kierowcy (zatwierdzenie/odrzucenie)

    Synchronizacja między dorosłym a dzieckiem (mutex + condition variable)

------------------------------------------------------------------

## Wykorzystane mechanizmy

## 1. Tworzenie i obsługa plików

### Tworzenie katalogu logów
- **`src/main.c:453`** - `mkdir(LOG_DIR, 0755)` - tworzenie katalogu przed startem
- **`src/logging.c:116`** - `mkdir(LOG_DIR, 0755)` - tworzenie w `log_init()`

### Otwieranie plików logów
- **`src/logging.c:88`** - `fopen(filename, "a")` - otwieranie w trybie append w `write_log_entry()`

### Zapisywanie do plików logów z blokadą
- **`src/logging.c:96-110`** - `flock(fd, LOCK_EX | LOCK_NB)` - blokada wyłączna, non-blocking z retry
- **`src/logging.c:108`** - `fprintf(f, "%s\n", entry)` - zapis do pliku
- **`src/logging.c:109`** - `fflush(f)` - wymuszenie zapisu
- **`src/logging.c:110`** - `flock(fd, LOCK_UN)` - zwolnienie blokady

### Zamykanie plików
- **`src/logging.c:112`** - `fclose(f)` - zamykanie pliku po zapisie

### Usuwanie starych logów
- **`src/main.c:460-465`** - `unlink(LOG_MASTER)`, `unlink(LOG_DISPATCHER)`, itd. - usuwanie przed startem

---

## 2. Tworzenie i obsługa procesów

### fork() i exec() dla dyspozytora
- **`src/main.c:89`** - `fork()` - tworzenie procesu potomnego
- **`src/main.c:98`** - `execl("./dispatcher", "dispatcher", NULL)` - uruchomienie dispatchera
- **`src/main.c:469`** - wywołanie `spawn_dispatcher()`

### fork() i exec() dla kasy
- **`src/main.c:108`** - `fork()` - tworzenie procesu potomnego
- **`src/main.c:119`** - `execl("./ticket_office", "ticket_office", id_str, NULL)` - uruchomienie kasy
- **`src/main.c:485-490`** - pętla tworząca wszystkie kasy

### fork() i exec() dla kierowcy
- **`src/main.c:129`** - `fork()` - tworzenie procesu potomnego
- **`src/main.c:140`** - `execl("./driver", "driver", id_str, NULL)` - uruchomienie kierowcy
- **`src/main.c:497-502`** - pętla tworząca wszystkich kierowców

### fork() i exec() dla pasażera
- **`src/main.c:150`** - `fork()` - tworzenie procesu potomnego
- **`src/main.c:159`** - `execl("./passenger", "passenger", NULL)` - uruchomienie pasażera
- **`src/main.c:538`** - wywołanie `spawn_passenger()` w pętli głównej

### Oczekiwanie na zakończenie procesów potomnych
- **`src/main.c:188-230`** - `reap_children()` - `waitpid(-1, &status, WNOHANG)` - non-blocking wait
- **`src/main.c:312-325`** - `waitpid(-1, &status, 0)` - blocking wait w `terminate_children()`
- **`src/main.c:328-371`** - `wait_all_children()` - timeout-based wait z `WNOHANG`

### Zabijanie procesów potomnych przy zakończeniu
- **`src/main.c:270-287`** - `kill(pid, SIGTERM)` - wysłanie SIGTERM do wszystkich dzieci
- **`src/main.c:292-310`** - `kill(pid, SIGKILL)` - wymuszenie zakończenia w `terminate_children()`

---

## 3. Tworzenie i obsługa wątków

### pthread_create() dla dziecka pasażera
- **`src/passenger.c:80`** - `pthread_create(&g_child_thread, NULL, child_thread_func, &g_info.child_age)` - tworzenie wątku dziecka

### pthread_join() dla dziecka
- **`src/passenger.c:104`** - `pthread_join(g_child_thread, NULL)` - oczekiwanie na zakończenie wątku

### pthread_mutex_lock()/unlock()
- **`src/passenger.c:24`** - deklaracja `pthread_mutex_t g_board_mutex = PTHREAD_MUTEX_INITIALIZER`
- **`src/passenger.c:54`** - `pthread_mutex_lock(&g_board_mutex)` - blokada przed `pthread_cond_wait`
- **`src/passenger.c:59`** - `pthread_mutex_unlock(&g_board_mutex)` - zwolnienie po `pthread_cond_wait`
- **`src/passenger.c:98-100`** - `pthread_mutex_lock/unlock` w `wait_for_child_thread()`
- **`src/passenger.c:330-333`** - `pthread_mutex_lock/unlock` przy sygnalizacji dziecku

### pthread_cond_wait()/signal()
- **`src/passenger.c:25`** - deklaracja `pthread_cond_t g_board_cond = PTHREAD_COND_INITIALIZER`
- **`src/passenger.c:57`** - `pthread_cond_wait(&g_board_cond, &g_board_mutex)` - oczekiwanie w wątku dziecka
- **`src/passenger.c:99`** - `pthread_cond_signal(&g_board_cond)` - sygnalizacja dziecku
- **`src/passenger.c:332`** - `pthread_cond_signal(&g_board_cond)` - sygnalizacja po wsiadaniu

---

## 4. Obsługa sygnałów

### sigaction() dla SIGUSR1/SIGUSR2
- **`src/dispatcher.c:65`** - `sigaction(SIGUSR1, &sa, NULL)` - rejestracja handlera dla early departure
- **`src/dispatcher.c:72`** - `sigaction(SIGUSR2, &sa, NULL)` - rejestracja handlera dla zamknięcia stacji
- **`src/main.c:79-80`** - `sigaction(SIGINT/SIGTERM, &sa, NULL)` - rejestracja shutdown handlerów
- **`src/ticket_office.c:32-36`** - `sigaction(SIGINT/SIGTERM, &sa, NULL)` - rejestracja shutdown handlerów
- **`src/driver.c:setup_signals()`** - podobnie dla kierowców
- **`src/passenger.c:setup_signals()`** - podobnie dla pasażerów

### Wysyłanie sygnałów do procesów (kill())
- **`src/main.c:63`** - `kill(g_dispatcher_pid, SIGTERM)` - w handlerze shutdown
- **`src/main.c:272`** - `kill(g_passenger_pids[i], SIGTERM)` - zabijanie pasażerów
- **`src/main.c:277`** - `kill(g_ticket_office_pids[i], SIGTERM)` - zabijanie kas
- **`src/main.c:282`** - `kill(g_driver_pids[i], SIGTERM)` - zabijanie kierowców
- **`src/dispatcher.c:151`** - `kill(driver_pid, sig)` - przekazywanie sygnałów do kierowców

### Przekazywanie sygnału SIGUSR1 do kierowców
- **`src/dispatcher.c:146-158`** - `forward_signal_to_drivers(shm, SIGUSR1)` - funkcja przekazująca sygnał
- **`src/dispatcher.c:167`** - wywołanie w `process_signals()` po otrzymaniu SIGUSR1

### Obsługa SIGCHLD dla procesów zombie
- **`src/dispatcher.c:93`** - `sigaction(SIGCHLD, &sa, NULL)` - rejestracja handlera
- **`src/dispatcher.c:52-54`** - `handle_sigchld()` - pusty handler (SA_NOCLDSTOP)
- **`src/main.c:83-85`** - podobnie w main.c

---

## 5. Synchronizacja procesów

### Tworzenie i inicjalizacja semaforów
- **`src/ipc.c:86`** - `semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600)` - tworzenie zestawu semaforów
- **`src/ipc.c:95-140`** - `semctl(g_semid, SEM_XXX, SETVAL, arg)` - inicjalizacja wartości semaforów
  - **Linia 95** - `SEM_SHM_MUTEX = 1`
  - **Linia 100** - `SEM_LOG_MUTEX = 1`
  - **Linia 105** - `SEM_STATION_ENTRY = 1`
  - **Linia 110** - `SEM_ENTRANCE_PASSENGER = 1`
  - **Linia 115** - `SEM_ENTRANCE_BIKE = 1`
  - **Linia 120** - `SEM_BOARDING_MUTEX = 1`
  - **Linia 125** - `SEM_BUS_READY = 0`
  - **Linia 130-135** - `SEM_TICKET_OFFICE(id) = 1` dla każdej kasy
  - **Linia 140** - `SEM_TICKET_QUEUE_SLOTS = MAX_TICKET_QUEUE_REQUESTS`
  - **Linia 145** - `SEM_BOARDING_QUEUE_SLOTS = MAX_BOARDING_QUEUE_REQUESTS`

### Operacje P() i V() na semaforach
- **`src/ipc.c:330-348`** - `sem_lock()` - operacja P() (dekrementacja) - `semop()` z `sem_op = -1` (blokujące, zwraca -1 przy EINTR/EIDRM/EINVAL)
- **`src/ipc.c:350-366`** - `sem_unlock()` - operacja V() (inkrementacja) - `semop()` z `sem_op = +1` (nie blokujące)
- **`src/ipc.c:368-381`** - `sem_getval()` - odczyt wartości semafora - `semctl(..., GETVAL)`
- **`src/ipc.c:383-396`** - `sem_setval()` - ustawienie wartości semafora - `semctl(..., SETVAL, arg)`

### Semafor mutex dla pamięci współdzielonej
- **`src/ipc.c:95`** - inicjalizacja `SEM_SHM_MUTEX = 1`
- **`src/ipc.c:sem_lock(SEM_SHM_MUTEX)`** - używany wszędzie przed dostępem do `shm_data_t`
- Przykłady użycia:
  - **`src/dispatcher.c:100-144`** - `init_shared_state()` - inicjalizacja z mutexem
  - **`src/passenger.c:436`** - inkrementacja `total_passengers_created` z mutexem
  - **`src/driver.c:216`** - inkrementacja `passengers_transported` z mutexem

### Semafor limitujący kolejkę żądań
- **`src/ipc.c:140`** - `SEM_TICKET_QUEUE_SLOTS = MAX_TICKET_QUEUE_REQUESTS` - limit requestów biletowych
- **`src/ipc.c:145`** - `SEM_BOARDING_QUEUE_SLOTS = MAX_BOARDING_QUEUE_REQUESTS` - limit requestów boardingowych
- **`src/passenger.c:182`** - `sem_lock(SEM_TICKET_QUEUE_SLOTS)` - przed wysłaniem requestu biletu
- **`src/passenger.c:225`** - `sem_unlock(SEM_TICKET_QUEUE_SLOTS)` - po otrzymaniu odpowiedzi
- **`src/passenger.c:299`** - `sem_lock(SEM_BOARDING_QUEUE_SLOTS)` - przed wysłaniem requestu boardingowego
- **`src/passenger.c:325`** - `sem_unlock(SEM_BOARDING_QUEUE_SLOTS)` - po otrzymaniu odpowiedzi

---

## 6. Pamięć dzielona

### Tworzenie segmentu pamięci współdzielonej
- **`src/ipc.c:71`** - `shmget(SHM_KEY, sizeof(shm_data_t), IPC_CREAT | 0600)` - tworzenie segmentu

### Przyłączanie pamięci współdzielonej
- **`src/ipc.c:77`** - `shmat(g_shmid, NULL, 0)` - przyłączenie w dispatcherze (twórcy)
- **`src/ipc.c:204`** - `shmat(g_shmid, NULL, 0)` - przyłączenie w innych procesach (`ipc_attach_all()`)

### Odłączanie pamięci współdzielonej
- **`src/ipc.c:37`** - `shmdt(g_shm)` - odłączenie w `ipc_cleanup_partial()`
- **`src/ipc.c:ipc_detach_all()`** - funkcja odłączająca pamięć we wszystkich procesach

### Usuwanie segmentu pamięci
- **`src/ipc.c:41`** - `shmctl(g_shmid, IPC_RMID, NULL)` - usuwanie segmentu
- **`src/ipc.c:ipc_cleanup_all()`** - funkcja czyszcząca wszystkie zasoby IPC

---

## 7. Kolejki komunikatów

### Tworzenie kolejek komunikatów
- **`src/ipc.c:142`** - `msgget(MSG_TICKET_KEY, IPC_CREAT | 0600)` - kolejka requestów biletowych
- **`src/ipc.c:147`** - `msgget(MSG_TICKET_RESP_KEY, IPC_CREAT | 0600)` - kolejka odpowiedzi biletowych
- **`src/ipc.c:152`** - `msgget(MSG_BOARDING_KEY, IPC_CREAT | 0600)` - kolejka requestów boardingowych
- **`src/ipc.c:157`** - `msgget(MSG_BOARDING_RESP_KEY, IPC_CREAT | 0600)` - kolejka odpowiedzi boardingowych
- **`src/ipc.c:162`** - `msgget(MSG_DISPATCH_KEY, IPC_CREAT | 0600)` - kolejka dispatchera

### Wysyłanie wiadomości (msgsnd())
- **`src/ipc.c:416-424`** - `msg_send_ticket()` - `msgsnd(g_msgid_ticket, msg, sizeof(...) - sizeof(long), 0)` - wysyłanie requestu biletu
- **`src/ipc.c:427-435`** - `msg_send_ticket_resp()` - `msgsnd(g_msgid_ticket_resp, ...)` - wysyłanie odpowiedzi biletu (dedykowana kolejka)
- **`src/ipc.c:454-462`** - `msg_send_boarding()` - `msgsnd(g_msgid_boarding, ...)` - wysyłanie requestu boardingowego
- **`src/ipc.c:465-473`** - `msg_send_boarding_resp()` - `msgsnd(g_msgid_boarding_resp, ...)` - wysyłanie odpowiedzi boardingowej (dedykowana kolejka)
- **`src/passenger.c:191`** - wywołanie `msg_send_ticket(&request)` - pasażer wysyła request biletu
- **`src/ticket_office.c:143`** - wywołanie `msg_send_ticket_resp(&response)` - kasa wysyła odpowiedź (mtype = PID pasażera)
- **`src/passenger.c:305`** - wywołanie `msg_send_boarding(&request)` - pasażer wysyła request boardingowy (mtype = MSG_BOARD_REQUEST lub MSG_BOARD_REQUEST_VIP dla VIP)
- **`src/driver.c:155`** - wywołanie `msg_send_boarding_resp(&response)` - kierowca wysyła odpowiedź (mtype = PID pasażera)

### Odbieranie wiadomości (msgrcv())
- **`src/ipc.c:437`** - `msg_recv_ticket()` - `msgrcv(g_msgid_ticket, msg, sizeof(...) - sizeof(long), mtype, flags)` - odbieranie requestu biletu
- **`src/ipc.c:446`** - `msg_recv_ticket_resp()` - `msgrcv(g_msgid_ticket_resp, ...)` - odbieranie odpowiedzi biletu
- **`src/ipc.c:475`** - `msg_recv_boarding()` - `msgrcv(g_msgid_boarding, ...)` - odbieranie requestu boardingowego
- **`src/ipc.c:484`** - `msg_recv_boarding_resp()` - `msgrcv(g_msgid_boarding_resp, ...)` - odbieranie odpowiedzi boardingowej
- **`src/ticket_office.c:273`** - wywołanie `msg_recv_ticket(&request, MSG_TICKET_REQUEST, 0)` - kasa odbiera request (blokujące)
- **`src/passenger.c:204`** - wywołanie `msg_recv_ticket_resp(&response, g_info.pid, 0)` - pasażer odbiera odpowiedź (blokujące, mtype = PID pasażera)
- **`src/driver.c:441`** - wywołanie `msg_recv_boarding(&request, -MSG_BOARD_REQUEST, 0)` - kierowca odbiera request (blokujące, **negatywny mtype** dla priorytetu VIP - najniższy typ pierwszy)
- **`src/passenger.c:313`** - wywołanie `msg_recv_boarding_resp(&response, g_info.pid, 0)` - pasażer odbiera odpowiedź (blokujące, mtype = PID pasażera)

### Oddzielne kolejki dla żądań i odpowiedzi
- **Kolejki requestów:**
  - `MSG_TICKET_KEY` - requesty biletowe (pasażer → kasa)
  - `MSG_BOARDING_KEY` - requesty boardingowe (pasażer → kierowca)
- **Kolejki odpowiedzi:**
  - `MSG_TICKET_RESP_KEY` - odpowiedzi biletowe (kasa → pasażer)
  - `MSG_BOARDING_RESP_KEY` - odpowiedzi boardingowe (kierowca → pasażer)
- **Definicje kluczy:** `include/config.h:MSG_TICKET_KEY`, `MSG_TICKET_RESP_KEY`, `MSG_BOARDING_KEY`, `MSG_BOARDING_RESP_KEY`

### Usuwanie kolejek
- **`src/ipc.c:49`** - `msgctl(g_msgid_ticket, IPC_RMID, NULL)` - usuwanie kolejki requestów biletowych
- **`src/ipc.c:53`** - `msgctl(g_msgid_ticket_resp, IPC_RMID, NULL)` - usuwanie kolejki odpowiedzi biletowych
- **`src/ipc.c:57`** - `msgctl(g_msgid_boarding, IPC_RMID, NULL)` - usuwanie kolejki requestów boardingowych
- **`src/ipc.c:61`** - `msgctl(g_msgid_boarding_resp, IPC_RMID, NULL)` - usuwanie kolejki odpowiedzi boardingowych
- **`src/ipc.c:65`** - `msgctl(g_msgid_dispatch, IPC_RMID, NULL)` - usuwanie kolejki dispatchera
- **`src/ipc.c:ipc_cleanup_all()`** - funkcja usuwająca wszystkie kolejki

---

## Dodatkowe informacje

### Struktury danych w pamięci współdzielonej
- **`include/common.h:54-85`** - definicja `shm_data_t` - główna struktura danych
- **`include/common.h:43-52`** - definicja `bus_state_t` - stan pojedynczego autobusu
- **`include/common.h:87-98`** - definicja `passenger_info_t` - informacje o pasażerze

### Klucze IPC
- **`include/config.h:IPC_KEY_BASE`** - bazowy klucz IPC (0x4255)
- **`include/config.h:SHM_KEY`** - klucz pamięci współdzielonej
- **`include/config.h:SEM_KEY`** - klucz semaforów
- **`include/config.h:MSG_TICKET_KEY`** - klucz kolejki requestów biletowych
- **`include/config.h:MSG_TICKET_RESP_KEY`** - klucz kolejki odpowiedzi biletowych
- **`include/config.h:MSG_BOARDING_KEY`** - klucz kolejki requestów boardingowych
- **`include/config.h:MSG_BOARDING_RESP_KEY`** - klucz kolejki odpowiedzi boardingowych

### Indeksy semaforów
- **`include/common.h:8-21`** - enum `SemaphoreIndex` - definicja wszystkich semaforów
- **`SEM_SHM_MUTEX = 0`** - mutex dla pamięci współdzielonej
- **`SEM_LOG_MUTEX = 1`** - mutex dla logów
- **`SEM_STATION_ENTRY = 2`** - kontrola wejścia na stację
- **`SEM_ENTRANCE_PASSENGER = 3`** - wejście pasażerskie
- **`SEM_ENTRANCE_BIKE = 4`** - wejście dla rowerów
- **`SEM_BOARDING_MUTEX = 5`** - mutex dla boarding
- **`SEM_BUS_READY = 6`** - gotowość busa
- **`SEM_TICKET_OFFICE_BASE = 7`** - baza dla semaforów kas (7, 8, ...)
- **`SEM_TICKET_QUEUE_SLOTS`** - limit requestów biletowych
- **`SEM_BOARDING_QUEUE_SLOTS`** - limit requestów boardingowych


## Testy - sa one przeprowadzane w tym samym czasie co symulacja. 
## Pozwala to sprawdzic czy symulacja sie zawiesza badz ma problem.
### 1. Test zabijający jednego z aktywnych kierowców
Wywołanie tego testu następuje poprzez wysłanie sygnału SIGKILL do aktywnego kierowcy.
```
$ kill -SIGKILL <PID_KIEROWCA>
```
lub
```console
$ ./main --test1
```
Po przeprowadzeniu testu program nadal przewozi pasazerow jezeli zostaje przynajmniej jeden aktywny autobus.

### 2. Test sprawdzający działanie sygnału SIGUSR2
Wywołanie tego testu następuje poprzez wysłanie sygnału SIGUSR2 do dispatchera. Użycie:
```console
$ kill -SIGUSR2 <PID_DISPATCHER>
```
lub
```console
$ ./main --test2
```

Wynik:
```
[DISPATCHER] SIGUSR2 received - station CLOSED (end simulation)
[DISPATCHER] SIGUSR2 processed - station closed, waiting passengers will be transported
[MAIN] Spawning stopped by dispatcher (station closed) or previous fork() error

[MAIN] Passenger creation stopped. Monitoring simulation...

STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=24825 waiting=364 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=342 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=302 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=259 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=195 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=137 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=68 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=22 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156
STATUS: created=25689 transported=25325 waiting=0 in_office=0 tickets=22156

[MAIN] Driver 1 terminated
[MAIN] Drain complete (spawning stopped)

[MAIN] Simulation complete. Shutting down...

[MAIN] Signaling dispatcher to shutdown...

[DISPATCHER] Shutdown signal received
STATUS: created=25689 transported=25689 waiting=0 in_office=0 tickets=22156

========== FINAL STATS ==========
Created people: 25689 (adults=22354, children=3337, vip_people=223)
Tickets issued: 22156 (people covered=25468, denied=0)
[MAIN] Terminating all child processes...
Boarded people: 25689 (vip_people=223)
Transported people: 25689
Left early (station closed): 2
Remaining: waiting=0 in_office=0
================================
```
### 3. Test sprawdzajacy wielokrotne uzycie sygnalu SIGUSR1
Wywołanie tego testu następuje poprzez wysłanie sygnału SIGUSR2 do dispatchera N ilość razy (standardowo ustawione jako 5). Użycie:
```console
$ kill -SIGUSR1 <PID_DISPATCHER>
```
lub
```console
$ ./main --test3
```
Po wykonaniu busy odjezdzaja jak nalezy z dworca, przetwarzają poprawnie zadanie.

### 4. Test zabijający jednego z aktywnych ticket offices.

Wywołanie tego testu następuje poprzez wysłanie sygnału SIGKILL do aktywnego ticket office.
```
$ kill -SIGKILL <PID_KIEROWCA>
```
lub
```console
$ ./main --test4
```
Po przeprowadzeniu testu program nadal sprzedaje bilety jezeli zostaje przynajmniej jeden aktywny ticket office.

### 5. Test sprawdzający prawidłowość zapisywanych statystyk
Test polega na sprawdzeniu poprawności wszystkich statystyk i wiarygodności danych

```console
$ ./main --test5
```

## Napotkane problemy
- W trybie --performance busy odjezdzaly tak szybko, ze nie mialy czasu zbierac pasazerow, teraz nawet w tym trybie czekaja sekunde.
- Aktywne busy mialy ten sam czas oczekiwania tzn. liczyly go od momentu aktywacji pierwszego busa, co sprawialo ze gdy odjezdzal jeden to kolejne busy zamiast czekac T ilosc czasu to odjezdzaly od razy. Teraz kazdy liczy czas odjazdu odkad staje sie aktywny
- Gdy VIP mial dziecko to byly problemy z boardingiem
- Problem synchronizacji dziecka z dorosłym. Dodalem watki z mutexem
- Zapychanie dworca przez pasazerow. Dodalem semafor SEM_STATION_ENTRY i semafory do wejscia do autobusu SEM_ENTRANCE_PASSENGER, SEM_ENTRANCE_BIKE.
- Wiele autobusow zamiast czekac przyjmowalo pasazerow. Teraz jest zmienna zmienna active_bus_id.
- Deadlock w kolejkach komunikatow. Rozdzielilem zatem to na kolejke do przyjmowania i na kolejke do odsylania.


## Kluczowe pseudokody
- Główna pętla pasazera
```
FUNKCJA main():
    Inicjalizuj pasażera (wiek, VIP, rower, dziecko)
    Jeśli ma dziecko < 8 lat:
        Utwórz wątek dziecka (pthread_create)
    
    Zwiększ shm->total_passengers_created
    
    Jeśli NIE jest VIP:
        Jeśli purchase_ticket() == 0:
            Zwiększ passengers_left_early
            Zakończ proces
    
    Jeśli stacja zamknięta:
        Zakończ proces
    
    enter_station()  // Wejście na stację
    
    PĘTLA (dopóki g_running):
        Sprawdź czy dispatcher żyje (check_dispatcher_alive)
        
        Jeśli attempt_boarding() == 1:  // Sukces
            Jeśli ma dziecko:
                Sygnalizuj wątek dziecka (pthread_cond_signal)
            Poczekaj na wątek dziecka (pthread_join)
            Zakończ proces
        
        Jeśli attempt_boarding() == -1:  // Odrzucono, czekaj
            Kontynuuj pętlę
        
        Jeśli attempt_boarding() == 0:  // Błąd krytyczny
            Zakończ proces
    
    Zakończ proces
```

- Zakup biletu

```
FUNKCJA purchase_ticket(shm):
    Zwiększ shm->passengers_in_office
    
    Jeśli stacja zamknięta:
        Zmniejsz passengers_in_office
        Zwróć 0
    
    Zablokuj SEM_TICKET_QUEUE_SLOTS  // Limit requestów
    
    Przygotuj request (mtype=MSG_TICKET_REQUEST, PID, wiek, rower, dziecko)
    Wyślij request (msg_send_ticket)
    
    Odbierz odpowiedź (msg_recv_ticket_resp, mtype=nasz_PID, blokujące)
    
    Zwolnij SEM_TICKET_QUEUE_SLOTS
    
    Jeśli odpowiedź.approved == true:
        Ustaw g_info.has_ticket = true
        Zwróć 1
    W przeciwnym razie:
        Zwróć 0
```

- Wejście na stację

```
FUNKCJA enter_station(shm):
    Zablokuj SEM_SHM_MUTEX
    Sprawdź shm->station_open
    Zwolnij SEM_SHM_MUTEX
    
    Jeśli stacja zamknięta:
        Zwróć 0
    
    Zablokuj SEM_STATION_ENTRY  // Kontrola wejścia
    
    Zablokuj SEM_SHM_MUTEX
    Jeśli shm->station_open == true:
        Zwiększ shm->passengers_waiting o seat_count
    Zwolnij SEM_SHM_MUTEX
    
    Zwolnij SEM_STATION_ENTRY
    
    Zwróć 1
```

- Próba wsiadania do autobusu

```
FUNKCJA attempt_boarding(shm):
    Zablokuj SEM_SHM_MUTEX
    active_bus = shm->active_bus_id
    boarding_allowed = shm->boarding_allowed
    Zwolnij SEM_SHM_MUTEX
    
    Jeśli active_bus < 0 LUB !boarding_allowed:
        Zwróć -1  // Czekaj
    
    Przygotuj request:
        mtype = (VIP ? MSG_BOARD_REQUEST_VIP : MSG_BOARD_REQUEST)
        bus_id = active_bus
        passenger = g_info
    
    Zablokuj SEM_BOARDING_QUEUE_SLOTS  // Limit requestów
    
    Wyślij request (msg_send_boarding)
    
    Odbierz odpowiedź (msg_recv_boarding_resp, mtype=nasz_PID, blokujące)
    
    Zwolnij SEM_BOARDING_QUEUE_SLOTS
    
    Jeśli odpowiedź.approved == true:
        Ustaw g_info.assigned_bus
        Jeśli ma dziecko:
            Sygnalizuj wątek dziecka (pthread_cond_signal)
        Zwróć 1
    W przeciwnym razie:
        Zwróć -1  // Czekaj na następny autobus
```


## Temat 12 – Autobus Podmiejski
Na dworcu stoi autobus o pojemności P pasażerów, w którym jednocześnie można przewieźć R
rowerów. Do autobusu są dwa wejścia (każde może pomieścić jedną osobę): jedno dla pasażerów z
bagażem podręcznym i drugie dla pasażerów z rowerami. Autobus odjeżdża co określoną ilość czasu
T (np.: co 10 minut). W momencie odjazdu kierowca musi dopilnować aby na wejściach nie było
żadnego wchodzącego pasażera. Jednocześnie musi dopilnować by liczba wszystkich pasażerów
autobusu nie przekroczyła P i liczba pasażerów z rowerami nie była większa niż R. Po odjeździe
autobusu na jego miejsce pojawia się natychmiast (jeżeli jest dostępny) nowy pojazd o takiej samej
pojemności jak poprzedni. Łączna liczba autobusów wynosi N, każdy o pojemności P z R miejscami
na rowery.
Pasażerowie w różnym wieku przychodzą na dworzec w losowych momentach czasu. Przed
wejściem na przystanek kupują bilet w kasie (K). Istnieje pewna liczba osób VIP (ok. 1% ) –
posiadająca wcześniej wykupiony bilet, które nie płacą za bilet i mogą wejść na przystanek i do
autobusu z pominięciem kolejki oczekujących. Dzieci w wieku poniżej 8 roku życia mogą wejść do
autobusu tylko pod opieką osoby dorosłej (dziecko zajmuje osobne miejsce w autobusie). Kasa
rejestruje wszystkie wchodzące osoby (ID procesu/wątku). Każdy pasażer przy wejściu do autobusu
okazuje kierowcy ważny bilet – do autobusu nie może wejść osoba bez wykupionego wcześniej biletu.
Autobusy przewożą pasażerów do miejsca docelowego i po czasie Ti (wartość losowa, każdy autobus
ma inny czas) wracają na dworzec. Na polecenie dyspozytora (sygnał 1) autobus, który w danym
momencie stoi na dworcu może odjechać z niepełną liczbą pasażerów. Po otrzymaniu od dyspozytora
polecenia (sygnał 2) pasażerowie nie mogą wsiąść do żadnego autobusu - nie mogą wejść na
dworzec. Autobusy kończą pracę po rozwiezieniu wszystkich pasażerów.
Napisz programy symulujące działanie dyspozytora, kasy, kierowcy i pasażerów. Raport z przebiegu
symulacji zapisać w pliku (plikach) tekstowym.