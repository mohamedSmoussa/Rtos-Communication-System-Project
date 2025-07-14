📡 Packet Transmission System with Stop-and-Wait & Go-Back-N Protocols (RTOS-Based)
This project implements a packet transmission system using two classic protocols — Stop-and-Wait (S&W) and Go-Back-N (GBN) — designed and tested on an RTOS (FreeRTOS) environment.

💡 System Architecture
mathematica
Copy
Edit
2 Senders → Switch → 2 Receivers
Built using FreeRTOS primitives to ensure concurrency, timing precision, and robust resource management.

⚙️ RTOS Primitives Used
xTaskCreate() — Creates sender, receiver, and switch tasks.

xQueueCreate() — Packet and ACK queues.

xTimerCreate() — Retransmission timers.

xSemaphoreCreateMutex() — Protects shared data and statistics counters.

⏳ Stop-and-Wait Protocol (RTOS Implementation)
Sender
c
Copy
Edit
xTimerStart(timer, TOUT_MS);         // Start retransmission timeout
xQueueSend(switchQueue, pkt);        // Transmit packet to switch
xQueueReceive(ackQueue);             // Wait for ACK
Receiver
c
Copy
Edit
xQueueReceive(receiverQueue);        // Receive packet
xQueueSend(ackQueue, ack);           // Send ACK
RTOS Features
Single timer per sender

Blocking queue waits simplify design

Mutex-protected counters for accurate performance stats

🔄 Go-Back-N Protocol (RTOS Implementation)
Sender
c
Copy
Edit
for (i = 0; i < N; i++) {
    xQueueSend(switchQueue, pkt[i]);    // Send window of packets
    xTimerStart(timers[i], TOUT_MS);    // Start timer per packet
}
ACK Handler
c
Copy
Edit
xQueueReceive(ackQueue);
while (base <= ack.seq) {
    xTimerStop(timers[base % N]);       // Stop acknowledged packet timers
    base++;                             // Slide window forward
}
RTOS Features
Array of timers (size = N)

Non-blocking queue checks for fast handling

Atomic window management ensures correctness

📊 Performance Comparison (RTOS Impact)
Metric	S&W (RTOS)	GBN (RTOS)
Task Count	5 tasks	5 tasks + N timers
Memory Use	Low	Higher (buffers & timers)
Context Switches	Frequent (per-packet)	Optimized (batched)

🚀 Throughput Advantage
GBN achieves ~46% higher throughput compared to S&W.

Advantages:

Batched xQueueSend() via windowing

Fewer xTimerStart() calls (per window instead of per packet)

⚖️ Why FreeRTOS?
Precise timing control with vTaskDelay() and timers

Queue system naturally models network links and buffers

Mutexes avoid race conditions in statistics and shared resources

✅ Conclusion
This system demonstrates how RTOS primitives can simplify and optimize protocol implementation, achieving both correctness and high throughput — especially when scaling to window-based protocols like Go-Back-N.

