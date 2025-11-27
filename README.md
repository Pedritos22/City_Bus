Temat 12 – Autobus podmiejski.
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
