#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define GBN_WINDOW_SIZE     4    // control window size from here N= ??
#define NUM_SENDERS         2
#define NUM_RECEIVERS       2
#define RECEIVER_ID_BASE    3
#define MIN_TOTAL_RECV_PER_RECEIVER  1000
#define MAX_RETRIES         4
#define P_DROP              20000   // control from here pd= 20000/10000  means 2%
#define P_ACK               10000
#define LINK_CAPACITY       100000
#define PROPAGATION_DELAY   5
#define TOUT_MS            150     // output timer
#define T1_MS               100
#define T2_MS               200
#define L1                  500
#define L2                  1500
#define ACK_SIZE            40
#define QUEUE_WAIT_TICKS    pdMS_TO_TICKS(100)
#define MUTEX_WAIT_TICKS    pdMS_TO_TICKS(50)
#define PACKET_POOL_SIZE  7
typedef struct {
    uint8_t destination;
    uint32_t seqNumber;
    uint16_t length;
    uint8_t senderId;
    char data[L2];
} Packet;
typedef struct {
    uint8_t ackSender;
    uint8_t ackDest;
    uint32_t seqNumber;
} AckPacket;
typedef struct {
    Packet *packets[GBN_WINDOW_SIZE];
    TimerHandle_t timers[GBN_WINDOW_SIZE];
    int retries[GBN_WINDOW_SIZE];
    int rIdx[GBN_WINDOW_SIZE];
    uint32_t base;
    uint32_t nextSeqNum;
} GBNWindow;
typedef struct {
    Packet *packets[PACKET_POOL_SIZE];
    QueueHandle_t freePoolQueue;
} PacketPool;
QueueHandle_t switchQueue;
QueueHandle_t ackQueues[NUM_SENDERS];
QueueHandle_t receiverQueues[NUM_RECEIVERS];
SemaphoreHandle_t recvCountMutex;
SemaphoreHandle_t simulationDoneSemaphore;
SemaphoreHandle_t sendBufferMutex[NUM_SENDERS];
SemaphoreHandle_t statsMutex;
uint32_t globalReceived = 0;
uint64_t totalBytesReceived = 0;
uint32_t sentCount[NUM_SENDERS][NUM_RECEIVERS] = {0};
uint32_t recvCount[NUM_SENDERS][NUM_RECEIVERS] = {0};
uint32_t retryCount[NUM_SENDERS] = {0};
uint32_t dropAfterMaxRetry[NUM_SENDERS] = {0};
uint32_t dropBySwitch[NUM_SENDERS] = {0};
uint32_t seqTracker[NUM_SENDERS][NUM_RECEIVERS] = {0};
uint32_t recvCountPerReceiver[NUM_RECEIVERS] = {0};
volatile int simulationComplete = 0;
TickType_t simulationStartTime;
GBNWindow gbnWindows[NUM_SENDERS];
PacketPool packetPool;
TaskHandle_t senderTaskHandles[NUM_SENDERS];
TaskHandle_t ackReceiverTaskHandles[NUM_SENDERS];
void initPacketPool() {
    packetPool.freePoolQueue = xQueueCreate(PACKET_POOL_SIZE, sizeof(Packet *));
    if (!packetPool.freePoolQueue) {
        printf("Failed to create packet pool queue\n");
        while (1);}
    for (int i = 0; i < PACKET_POOL_SIZE; i++) {
        packetPool.packets[i] = (Packet *)pvPortMalloc(sizeof(Packet));
        if (!packetPool.packets[i]) {
            printf("Failed to allocate packet %d for pool\n", i);
            while (1); }
        Packet *pkt = packetPool.packets[i];
        if (xQueueSend(packetPool.freePoolQueue, &pkt, 0) != pdPASS) {
            printf("Failed to add packet %d to pool queue\n", i);
            while (1);}   }}
Packet *allocPacket() {
    Packet *pkt;
    if (xQueueReceive(packetPool.freePoolQueue, &pkt, 0) == pdPASS) {
        return pkt;}
    return NULL;}
void freePacket(Packet *pkt) {
    if (pkt) {
        if (xQueueSend(packetPool.freePoolQueue, &pkt, 0) != pdPASS) {
            printf("Failed to return packet to pool\n"); }}}
void cleanupPacket(Packet **pkt) {
    if (pkt && *pkt) {
        freePacket(*pkt);
        *pkt = NULL; }}
void printStats() {
    if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) != pdTRUE) {
        printf("Warning: Could not acquire stats mutex for printing\n");
        return; }
    printf("\n======= GPN Protocol Summary( WINDOW_SIZE = % u ) =======\n", GBN_WINDOW_SIZE);
    for (int s = 0; s < NUM_SENDERS; s++) {
        printf("Sender %d:\n", s);
        for (int r = 0; r < NUM_RECEIVERS; r++) {
            printf("  -> Receiver %d: Sent=%u, Received=%u\n", r + RECEIVER_ID_BASE, sentCount[s][r], recvCount[s][r]); }
        printf("  Retransmissions: %u\n", retryCount[s]);
        printf("  Dropped after max retries: %u\n", dropAfterMaxRetry[s]);
        printf("  Dropped by switch: %u\n", dropBySwitch[s]); }
    printf("===========================================\n");
    xSemaphoreGive(statsMutex);}
int allReceiversReachedMin() {
    if (xSemaphoreTake(recvCountMutex, MUTEX_WAIT_TICKS) != pdTRUE) {
        return 0;}
    int allDone = 1;
    for (int i = 0; i < NUM_RECEIVERS; i++) {
        if (recvCountPerReceiver[i] < MIN_TOTAL_RECV_PER_RECEIVER) {
            allDone = 0;
            break;}}
    xSemaphoreGive(recvCountMutex);
    return allDone;}
void senderTimeoutCallback(TimerHandle_t xTimer) {
    uint32_t tid = (uint32_t)(intptr_t)pvTimerGetTimerID(xTimer);
    int sid = tid >> 8;
    int index = tid & 0xFF;
    if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) return;
    GBNWindow *win = &gbnWindows[sid];
    for (uint32_t seq = win->base; seq < win->nextSeqNum; seq++) {
        int winIdx = seq % GBN_WINDOW_SIZE;
        if (!win->packets[winIdx]) continue;
        if (++win->retries[winIdx] >= MAX_RETRIES) {
            printf("[Sender %d] Max retries reached. Dropping packet %lu\n", sid, (unsigned long)win->packets[winIdx]->seqNumber);
            dropAfterMaxRetry[sid]++;
            sentCount[sid][win->rIdx[winIdx]]--;
            cleanupPacket(&win->packets[winIdx]);
            continue;  }
        retryCount[sid]++;
        sentCount[sid][win->rIdx[winIdx]]++;
        printf("[Sender %d] Retrying packet %lu | Attempt %d\n", sid, (unsigned long)win->packets[winIdx]->seqNumber, win->retries[winIdx]);
        Packet *pktPtr = win->packets[winIdx];
        xQueueSend(switchQueue, &pktPtr, QUEUE_WAIT_TICKS);
        xTimerStart(win->timers[winIdx], 0); }
    xSemaphoreGive(sendBufferMutex[sid]);}
void senderTask(void *param) {
    int sid = (int)(intptr_t)param;
    gbnWindows[sid].base = 0;
    gbnWindows[sid].nextSeqNum = 0;
    for (int i = 0; i < GBN_WINDOW_SIZE; i++) {
        gbnWindows[sid].packets[i] = NULL;
        gbnWindows[sid].timers[i] = xTimerCreate("GBNTimer", pdMS_TO_TICKS(TOUT_MS), pdFALSE, (void *)((sid << 8) | i), senderTimeoutCallback);
       gbnWindows[sid].retries[i] = 0;  }
    while (1) {
        if (simulationComplete) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue; }
        if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue; }
        if ((gbnWindows[sid].nextSeqNum - gbnWindows[sid].base) >= GBN_WINDOW_SIZE) {
            xSemaphoreGive(sendBufferMutex[sid]);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;  }
        xSemaphoreGive(sendBufferMutex[sid]);
        vTaskDelay(pdMS_TO_TICKS(T1_MS + rand() % (T2_MS - T1_MS + 1)));
        int destIdx = rand() % NUM_RECEIVERS;
        int dest = RECEIVER_ID_BASE + destIdx;
        uint16_t len = L1 + rand() % (L2 - L1 + 1);
        Packet *pkt = allocPacket();
        if (!pkt) {
            printf("[Sender %d] Packet allocation failed\n", sid);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue; }
        pkt->senderId = sid;
        pkt->destination = dest;
        pkt->length = len;
        if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) == pdTRUE) {
            pkt->seqNumber = gbnWindows[sid].nextSeqNum;
            sentCount[sid][destIdx]++;
            printf("[Sender %d] Sending packet %u to Receiver %d\n", sid, sentCount[sid][destIdx] - 1, dest);
            xSemaphoreGive(statsMutex);
        } else {
            freePacket(pkt);
            continue;  }
        if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) {
            freePacket(pkt);
            continue;   }
        int winIdx = gbnWindows[sid].nextSeqNum % GBN_WINDOW_SIZE;
        gbnWindows[sid].packets[winIdx] = pkt;
        gbnWindows[sid].retries[winIdx] = 0;
        gbnWindows[sid].rIdx[winIdx] = destIdx;
        gbnWindows[sid].nextSeqNum++;
        Packet *pktPtr = pkt;
        if (xQueueSend(switchQueue, &pktPtr, QUEUE_WAIT_TICKS) != pdPASS) {
            printf("[Sender %d] Failed to send to switch queue\n", sid);
            cleanupPacket(&gbnWindows[sid].packets[winIdx]);
            xSemaphoreGive(sendBufferMutex[sid]);
            continue;}
        xTimerStart(gbnWindows[sid].timers[winIdx], 0);
        xSemaphoreGive(sendBufferMutex[sid]);   }}
void ackReceiverTask(void *param) {
    int sid = (int)(intptr_t)param;
    AckPacket ack;
    while (1) {
        if (xQueueReceive(ackQueues[sid], &ack, portMAX_DELAY) == pdPASS) {
            if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) continue;
            GBNWindow *win = &gbnWindows[sid];
            if (ack.seqNumber >= win->base && ack.seqNumber < win->nextSeqNum) {
                while (win->base <= ack.seqNumber) {
                    int winIdx = win->base % GBN_WINDOW_SIZE;
                    if (win->packets[winIdx]) {
                        xTimerStop(win->timers[winIdx], 0);
                        cleanupPacket(&win->packets[winIdx]); }
                    win->base++;  }}
            xSemaphoreGive(sendBufferMutex[sid]);  }}}
void switchTask(void *param) {
    Packet *pkt;
    while (1) {
        if (xQueueReceive(switchQueue, &pkt, portMAX_DELAY) == pdPASS) {
            int drop = rand() % 1000000;
            if (drop < P_DROP) {
                printf("[Switch] Dropped packet %lu to R%d\n", (unsigned long)pkt->seqNumber, pkt->destination);
                dropBySwitch[pkt->senderId]++;
                cleanupPacket(&pkt);
                continue; }
            uint32_t t_delay = (pkt->length * 8 * 1000) / LINK_CAPACITY;
            vTaskDelay(pdMS_TO_TICKS(PROPAGATION_DELAY + t_delay));
            int rIdx = pkt->destination - RECEIVER_ID_BASE;
            if (rIdx >= 0 && rIdx < NUM_RECEIVERS) {
                if (xQueueSend(receiverQueues[rIdx], &pkt, QUEUE_WAIT_TICKS) != pdPASS) {
                    printf("[Switch] Failed to send to receiver %d\n", pkt->destination);
                    cleanupPacket(&pkt);}
            } else {
                printf("[Switch] Invalid receiver index %d\n", rIdx);
                cleanupPacket(&pkt);  }  } }}
void receiverTask(void *param) {
    int rIdx = (int)(intptr_t)param;
    int rid = rIdx + RECEIVER_ID_BASE;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(receiverQueues[rIdx], &pkt, portMAX_DELAY) == pdPASS) {
            int sid = pkt->senderId;
            uint32_t seqNum = pkt->seqNumber;
            recvCount[sid][rIdx]++;
            globalReceived++;
            recvCountPerReceiver[rIdx]++;
            totalBytesReceived += pkt->length;
            printf("[Receiver %d] Received packet %lu from S%d | Total: %u\n",
                   rid, (unsigned long)seqNum, sid, globalReceived);
            cleanupPacket(&pkt);
            int drop = rand() % 1000000;
            if (drop >= P_ACK) {
                uint32_t ack_delay = (ACK_SIZE * 8 * 1000) / LINK_CAPACITY;
                vTaskDelay(pdMS_TO_TICKS(PROPAGATION_DELAY + ack_delay));
                AckPacket ack = {
                    .ackSender = rid,
                    .ackDest = sid,
                    .seqNumber = seqNum
                };

                if (xQueueSend(ackQueues[sid], &ack, QUEUE_WAIT_TICKS) != pdPASS) {
                    printf("[Receiver %d] Failed to send ACK\n", rid);
                }
            } else {
                printf("[Receiver %d] Dropped ACK for S%d | Seq %lu\n", rid, sid, (unsigned long)seqNum);
            }

            if (allReceiversReachedMin() && !simulationComplete) {
                simulationComplete = 1;
                xSemaphoreGive(simulationDoneSemaphore);   } }}}
void simulationMonitorTask(void *param) {
    xSemaphoreTake(simulationDoneSemaphore, portMAX_DELAY);
    TickType_t simulationEndTime = xTaskGetTickCount();
    uint32_t totalTimeMs = (simulationEndTime - simulationStartTime) * portTICK_PERIOD_MS;
    printStats();
    printf("Total packets dropped after 4 tries: %u packet \n", dropAfterMaxRetry[0]+dropAfterMaxRetry[1]);
    printf("Total simulation time: %u ms\n", totalTimeMs);
    if (totalTimeMs > 0) {
        uint32_t throughputBytesPerSec = (totalBytesReceived * 1000) / totalTimeMs;
        printf("Throughput: %u bytes/sec\n", throughputBytesPerSec);
    }
    for (int i = 0; i < NUM_SENDERS; i++) {
        if (xSemaphoreTake(sendBufferMutex[i], portMAX_DELAY) == pdTRUE) {
            for (int j = 0; j < GBN_WINDOW_SIZE; j++) {
                if (gbnWindows[i].timers[j]) {
                    xTimerStop(gbnWindows[i].timers[j], 0);
                    xTimerDelete(gbnWindows[i].timers[j], 0);
                    gbnWindows[i].timers[j] = NULL;
                }
                cleanupPacket(&gbnWindows[i].packets[j]);
            }
            xSemaphoreGive(sendBufferMutex[i]);
        }

        vTaskSuspend(senderTaskHandles[i]);
        vTaskSuspend(ackReceiverTaskHandles[i]);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));}}
int main(void) {
    srand(12345);
    initPacketPool();
    switchQueue = xQueueCreate(20, sizeof(Packet *));
    for (int i = 0; i < NUM_RECEIVERS; i++) {
        receiverQueues[i] = xQueueCreate(20, sizeof(Packet *));
    }
    for (int i = 0; i < NUM_SENDERS; i++) {
        ackQueues[i] = xQueueCreate(20, sizeof(AckPacket));
    }
    recvCountMutex = xSemaphoreCreateMutex();
    simulationDoneSemaphore = xSemaphoreCreateBinary();
    statsMutex = xSemaphoreCreateMutex();
    for (int i = 0; i < NUM_SENDERS; i++) {
        sendBufferMutex[i] = xSemaphoreCreateMutex();
    }
    xTaskCreate(switchTask, "Switch", 2048, NULL, 3, NULL);
    for (int i = 0; i < NUM_RECEIVERS; i++) {
        xTaskCreate(receiverTask, "Receiver", 2048, (void *)(intptr_t)i, 1, NULL);
    }
    for (int i = 0; i < NUM_SENDERS; i++) {
        xTaskCreate(senderTask, "Sender", 2048, (void *)(intptr_t)i, 2, &senderTaskHandles[i]);
        xTaskCreate(ackReceiverTask, "AckRecv", 1024, (void *)(intptr_t)i, 2, &ackReceiverTaskHandles[i]);
    }
    xTaskCreate(simulationMonitorTask, "SimMonitor", 2048, NULL, 4, NULL);
    puts("Starting Send & Wait simulation...");
    simulationStartTime = xTaskGetTickCount();
    vTaskStartScheduler();
    while (1);}
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
void vApplicationMallocFailedHook(void) {
    puts("Malloc failed!");
    while (1);
}
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    printf("Stack overflow in task %s\n", pcTaskName);
    while (1);
}
