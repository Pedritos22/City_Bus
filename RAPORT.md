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
$ ./main --perf             # Tryb wydajnościowy (bez opóźnień symulacyjnych)
$ ./main --full             # Autobusy odjeżdżają gdy są pełne
$ ./main --max_p            # Ilość stworzonych pasazerow, zdefiniowana w config.h jako MAX_PASSENGER
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
[Tworzenie katalogu przed startem](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L560-L564)\
[Tworzenie w `log_init()`](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/logging.c#L115-L121)

### Otwieranie plików logów


### Zapisywanie do plików logów
[Zapisywanie do plików](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/logging.c#L87-L113)


### Usuwanie starych logów
[Usuwanie stary logów](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L566-L573)

---

## 2. Tworzenie i obsługa procesów

### fork() i exec() dla dyspozytora
[Funkcja spawn_dispatcher()](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L89-L106)\
[Wywołanie spawn_dispatcher()](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L577)

### fork() i exec() dla kasy
[Funkcja spawn_ticket_office](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L108-L123)\
[Pętla tworząca wszystkie kasy](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L592-L598)

### fork() i exec() dla kierowcy
[Funkcja spawn_driver](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L129-L148)\
[Pętla tworząca wszystkich kierowców](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L604-L610)

### fork() i exec() dla pasażera
[Funkcja spawn passenger](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L150-L166)\
[Pętla tworząca pasazerow](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L659-L684)

### Handling procesów potomnych
[Zakonczenie procesow potomnych](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L269-L373)

---

## 3. Tworzenie i obsługa wątków
[Funkcje dla child thread](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/passenger.c#L47-L105)\
[Przekazanie boardingu Child Thread](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/passenger.c#L329-L333)
---

## 4. Obsługa sygnałów

[Sygnaly dla dyspozytora](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/dispatcher.c#L19-L98)\
[Sygnaly w mainie](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/main.c#L56-L87)\
[Sygnaly w ticket_office](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/ticket_office.c#L19-L38)\
[Sygnaly w passenger](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/passenger.c#L29-L43)
###

---

## 5. Mechanizmy IPC

### Tworzenie i inicjalizacja IPC
[Funkcja ipc_create_all](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/ipc.c#L70-L195)

### Cleanup
[IPC cleanup](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/ipc.c#L259-L311)

### Operacje P() i V() na semaforach
[Funkcje sem_lock,sem_unlock,sem_getval,sem_setval](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/ipc.c#L330-L396)

### Dołączenie zasobów do IPC
[Funkcja ipc_attach_all](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/ipc.c#L197-L248)
### Odłączenie pamięci współdzielonej
[Funkcja ipc_detach_all](https://github.com/Pedritos22/City_Bus/blob/d373905f175ec19c021f5dacd25584ddcdaf76db/src/ipc.c#L250-L257)
---

### 

### Oddzielne kolejki dla żądań i odpowiedzi
- **Kolejki requestów:**
  - `MSG_TICKET_KEY` - requesty biletowe (pasażer → kasa)
  - `MSG_BOARDING_KEY` - requesty boardingowe (pasażer → kierowca)
- **Kolejki odpowiedzi:**
  - `MSG_TICKET_RESP_KEY` - odpowiedzi biletowe (kasa → pasażer)
  - `MSG_BOARDING_RESP_KEY` - odpowiedzi boardingowe (kierowca → pasażer)
- **Definicje kluczy:** `include/config.h:MSG_TICKET_KEY`, `MSG_TICKET_RESP_KEY`, `MSG_BOARDING_KEY`, `MSG_BOARDING_RESP_KEY`

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
[TEST1](https://github.com/Pedritos22/City_Bus/blob/c2df74f59b310ee831d8d59c522343c93a60931d/src/main.c#L380-L398)
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
[TEST2](https://github.com/Pedritos22/City_Bus/blob/c2df74f59b310ee831d8d59c522343c93a60931d/src/main.c#L400-L410)
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
[TEST3](https://github.com/Pedritos22/City_Bus/blob/c2df74f59b310ee831d8d59c522343c93a60931d/src/main.c#L412-L424)
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
[TEST4](https://github.com/Pedritos22/City_Bus/blob/c2df74f59b310ee831d8d59c522343c93a60931d/src/main.c#L426-L438)
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
[TEST5](https://github.com/Pedritos22/City_Bus/blob/c2df74f59b310ee831d8d59c522343c93a60931d/src/main.c#L440-L469)
Test polega na sprawdzeniu poprawności wszystkich statystyk i wiarygodności danych

```console
$ ./main --test5
```

### 6. Test pełnej kolejki biletowej
[TEST6](https://github.com/Pedritos22/City_Bus/blob/e54f43eceab2ffe776e354152896518fcb72ef29/src/main.c#L480-L545)
Test polega na zapchaniu całej kolejki do kasy biletowej poprzez ustawienie SEM_TICKET_QUEUE_SLOTS na 0.

```console
$ ./main --test6
```

### 7. Test pełnej kolejki do busa.
[TEST7](https://github.com/Pedritos22/City_Bus/blob/e54f43eceab2ffe776e354152896518fcb72ef29/src/main.c#L547-L616)
Test polega na zapchaniu całej kolejki do busa poprzez ustawienie SEM_BOARDING_QUEUE_SLOTS na 0.

```console
$ ./main --test7
```

### 8. Test pełnej kolejki biletowej oraz busa w tym samym momencie.
[TEST8](https://github.com/Pedritos22/City_Bus/blob/e54f43eceab2ffe776e354152896518fcb72ef29/src/main.c#L618-L710)
Test polega na zapchaniu obu kolejek poprzez ustawienie SEM_TICKET_QUEUE_SLOTS oraz SEM_BOARDING_QUEUE_SLOTS na 0.

```console
$ ./main --test8
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

- Przetwarzanie requestu boardingowego

```
FUNKCJA process_boarding_request(shm, request):
    Przygotuj response (mtype=request->passenger.pid)
    
    Zablokuj SEM_SHM_MUTEX
    
    Jeśli can_board(shm, request, reason) == true:
        response.approved = true
        
        Zwiększ bus->entering_count
        Zwolnij SEM_SHM_MUTEX
        
        Zablokuj entrance_sem (SEM_ENTRANCE_BIKE lub SEM_ENTRANCE_PASSENGER)
        
        
        Zablokuj SEM_SHM_MUTEX
        Zwiększ bus->passenger_count o seat_count
        Jeśli ma rower:
            Zwiększ bus->bike_count
        Zmniejsz bus->entering_count
        Zmniejsz shm->passengers_waiting o seat_count
        Zwiększ shm->boarded_people o seat_count
        Jeśli VIP:
            Zwiększ shm->boarded_vip_people o seat_count
        Zwolnij SEM_SHM_MUTEX
        
        Zwolnij entrance_sem
        
        Zaloguj sukces (z priorytetem VIP jeśli dotyczy)
    W przeciwnym razie:
        response.approved = false
        response.reason = powód odmowy
        Zwolnij SEM_SHM_MUTEX
    
    Wyślij response (msg_send_boarding_resp)
```

- Sprawdzenie możliwości wsiadania

```
FUNKCJA can_board(shm, request, reason):
    bus = shm->buses[g_bus_id]
    
    Jeśli bus->at_station == false:
        reason = "Bus not at station"
        Zwróć 0
    
    Jeśli bus->boarding_open == false:
        reason = "Bus boarding not open"
        Zwróć 0
    
    seats_needed = request->passenger.seat_count
    
    Jeśli bus->passenger_count + seats_needed > BUS_CAPACITY:
        reason = "Not enough seats"
        Zwróć 0
    
    Jeśli request->passenger.has_bike I bus->bike_count >= BIKE_CAPACITY:
        reason = "Bus at bicycle capacity"
        Zwróć 0
    
    Zwróć 1  // Można wsiadać
```

- Odjazd autobusu

```
FUNKCJA depart_bus(shm):
    bus = shm->buses[g_bus_id]
    
    Poczekaj aż wejścia są puste (wait_for_entrance_clear)
    
    Zablokuj SEM_SHM_MUTEX
    bus->boarding_open = false
    bus->at_station = false
    
    return_delay = losowa wartość (MIN_RETURN_TIME .. MAX_RETURN_TIME)
    bus->return_time = time() + return_delay
    
    passengers = bus->passenger_count
    bikes = bus->bike_count
    
    Zwiększ shm->passengers_transported o passengers
    bus->passenger_count = 0
    bus->bike_count = 0
    
    Zwolnij SEM_SHM_MUTEX
    
    Zaloguj odjazd
    
    sleep(return_delay)  // Symulacja podróży, chyba ze --perf mode
    
    Zablokuj SEM_SHM_MUTEX
    bus->at_station = true
    bus->boarding_open = true
    bus->departure_time = time() + BOARDING_INTERVAL
    
    Jeśli active_bus_id < 0 LUB active_bus nie jest na stacji:
        active_bus_id = g_bus_id
    
    Zwolnij SEM_SHM_MUTEX
    
    Zaloguj powrót
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