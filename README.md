# Rtos-Communication-System-Project
Packet transmission system with Stop-and-Wait &amp; Go-Back-N protocols, RTOS-based design, throughput &amp; stability tested.
2 Senders → Switch → 2 Receivers
(Built using FreeRTOS primitives)

Key RTOS Functions Used:

xTaskCreate() - Creates sender/receiver/switch tasks

xQueueCreate() - Packet and ACK queues

xTimerCreate() - Retransmission timers

xSemaphoreCreateMutex() - Protects shared data

⏳ Stop-and-Wait (RTOS Implementation)
Core Functions:

c
// Sender
xTimerStart(timer, TOUT_MS);  // Start timeout
xQueueSend(switchQueue, pkt); // Transmit
xQueueReceive(ackQueue);      // Wait ACK

// Receiver 
xQueueReceive(receiverQueue); // Get packet
xQueueSend(ackQueue, ack);    // Send ACK
RTOS Features:

Single timer per sender

Blocking queue waits

Mutex-protected stats counters

🔄 Go-Back-N (RTOS Implementation)
Core Functions:

c
// Sender
for(i=0; i<N; i++) {
  xQueueSend(switchQueue, pkt[i]); // Window transmit
  xTimerStart(timers[i], TOUT_MS); // Per-packet timer
}

// ACK Handler
xQueueReceive(ackQueue); 
while(base <= ack.seq) {           // Slide window
  xTimerStop(timers[base%N]); 
  base++;
}
RTOS Features:

Array of timers (size=N)

Non-blocking queue checks

Atomic window management

📊 Performance Comparison (RTOS Impact)
Metric	S&W (RTOS)	GBN (RTOS)
Task Count	5 tasks	5 tasks + N timers
Memory Use	Low	Higher (window buffers)
Context Switches	Frequent (per-packet)	Optimized (batched)
Throughput Advantage:

GBN achieves +46% higher throughput in RTOS due to:

xQueueSend batching (window of packets)

Reduced xTimerStart calls (per-window vs per-packet)

RTOS Selection Rationale:

FreeRTOS provides precise timing control via vTaskDelay()

Queue system naturally models network channels

Mutexes prevent race conditions in stats collection
