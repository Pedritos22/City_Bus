Topic 12 – Suburban Bus.

At the station, there is a bus with a capacity of P passengers, which can simultaneously carry R bicycles. The bus has two entrances (each can accommodate one person): one for passengers with carry-on luggage and one for passengers with bicycles.

The bus departs at regular intervals T (e.g., every 10 minutes). At the moment of departure, the driver must ensure that no passenger is entering through the doors. At the same time, the driver must ensure that the total number of passengers does not exceed P, and the number of passengers with bicycles does not exceed R.

After the bus departs, a new bus with the same capacity immediately takes its place if one is available. The total number of buses is N, each with capacity P and R bicycle spaces.

Passengers of different ages arrive at the station at random times. Before entering the bus stop, they buy a ticket at the ticket office (K). There is a certain number of VIP passengers (about 1%) who already have a ticket; they do not pay for a ticket and can enter the bus stop and the bus, bypassing the waiting queue.

Children under the age of 8 may only board the bus under the supervision of an adult (the child occupies a separate seat on the bus).

The ticket office registers all entering passengers (process/thread ID). Each passenger must show a valid ticket to the driver when boarding – a person without a previously purchased ticket cannot board the bus.

Buses transport passengers to their destinations and return to the station after a time Ti (a random value; each bus has a different time).

Upon the dispatcher’s command (signal 1), a bus currently at the station may depart with fewer than full capacity. Upon receiving the dispatcher’s command (signal 2), passengers cannot board any bus – they cannot enter the station.

Buses finish operating after transporting all passengers.

Write programs simulating the operation of the dispatcher, ticket office, driver, and passengers. Save a report of the simulation in a text file (or files).
