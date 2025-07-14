#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define NUM_SENDERS         2
#define NUM_RECEIVERS       2
#define RECEIVER_ID_BASE    3
#define MIN_TOTAL_RECV_PER_RECEIVER  200   // target per reciver
#define MAX_RETRIES         4
#define P_DROP              80000    // drop percentage = 80000 /10000 = 8%
#define P_ACK               10000
#define LINK_CAPACITY       100000
#define PROPAGATION_DELAY   5
#define TOUT_MS             175    // tout
#define T1_MS               100
#define T2_MS               200
#define L1                  500
#define L2                  1500
#define ACK_SIZE            40
#define QUEUE_WAIT_TICKS    pdMS_TO_TICKS(100)
#define MUTEX_WAIT_TICKS    pdMS_TO_TICKS(50)
#define PACKET_POOL_SIZE    20
typedef struct {
    uint8_t destination;
    uint32_t seqNumber;
    uint16_t length;
    uint8_t senderId;
    char data[L2];  // Fixed size for max packet length
} Packet;
typedef struct {
    uint8_t ackSender;
    uint8_t ackDest;
    uint32_t seqNumber;
} AckPacket;
typedef struct {
    Packet *pkt;
    int retries;
    TimerHandle_t timer;
    int rIdx;
} SendBuffer;
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
uint32_t total_tr;
volatile int simulationComplete = 0;
TickType_t simulationStartTime;
SendBuffer sendBuffers[NUM_SENDERS];
PacketPool packetPool;
TaskHandle_t senderTaskHandles[NUM_SENDERS];
TaskHandle_t ackReceiverTaskHandles[NUM_SENDERS];
void initPacketPool() {
    packetPool.freePoolQueue = xQueueCreate(PACKET_POOL_SIZE, sizeof(Packet *));
    if (!packetPool.freePoolQueue) {
        printf("Failed to create packet pool queue\n");
        while (1);
    }
    for (int i = 0; i < PACKET_POOL_SIZE; i++) {
        packetPool.packets[i] = (Packet *)pvPortMalloc(sizeof(Packet));
        if (!packetPool.packets[i]) {
            printf("Failed to allocate packet %d for pool\n", i);
            while (1);
        }
        Packet *pkt = packetPool.packets[i];
        if (xQueueSend(packetPool.freePoolQueue, &pkt, 0) != pdPASS) {
            printf("Failed to add packet %d to pool queue\n", i);
            while (1);
        }
    }
}
Packet *allocPacket() {
    Packet *pkt;
    if (xQueueReceive(packetPool.freePoolQueue, &pkt, 0) == pdPASS) {
        return pkt;
    }
    return NULL;
}
void freePacket(Packet *pkt) {
    if (pkt) {
        if (xQueueSend(packetPool.freePoolQueue, &pkt, 0) != pdPASS) {
            printf("Failed to return packet to pool\n");
        }
    }
}
void cleanupPacket(Packet **pkt) {
    if (pkt && *pkt) {
        freePacket(*pkt);
        *pkt = NULL;
    }
}
void printStats() {
    if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) != pdTRUE) {
        printf("Warning: Could not acquire stats mutex for printing\n");
        return;
    }
    printf("\n======= Send & Wait Protocol Summary =======\n");
    for (int s = 0; s < NUM_SENDERS; s++) {
        printf("Sender %d:\n", s);
        for (int r = 0; r < NUM_RECEIVERS; r++) {
            printf("  -> Receiver %d: Sent=%u, Received=%u\n",
                   r + RECEIVER_ID_BASE, sentCount[s][r], recvCount[s][r]);
        }
        printf("  Retransmissions: %u\n", retryCount[s]);
        printf("  Dropped after max retries: %u\n", dropAfterMaxRetry[s]);
        printf("  Dropped by switch: %u\n", dropBySwitch[s]);
    }
    printf("===========================================\n");
    xSemaphoreGive(statsMutex);
}
int allReceiversReachedMin() {
    if (xSemaphoreTake(recvCountMutex, MUTEX_WAIT_TICKS) != pdTRUE) {
        return 0;
    }
    int allDone = 1;
    for (int i = 0; i < NUM_RECEIVERS; i++) {
        if (recvCountPerReceiver[i] < MIN_TOTAL_RECV_PER_RECEIVER) {
            allDone = 0;
            break;
        }}
    xSemaphoreGive(recvCountMutex);
    return allDone;
}
void senderTimeoutCallback(TimerHandle_t xTimer) {
    int sid = (int)(intptr_t)pvTimerGetTimerID(xTimer);
    if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) {
        printf("[Sender %d] Could not acquire buffer mutex in timeout\n", sid);
        return;
    }
    SendBuffer *buf = &sendBuffers[sid];
    if (!buf->pkt) {
        xSemaphoreGive(sendBufferMutex[sid]);
        return;
    }
    if (++buf->retries >= MAX_RETRIES) {
        printf("[Sender %d] Max retries reached. Dropping packet %lu\n", sid, (unsigned long)buf->pkt->seqNumber);
        if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) == pdTRUE) {
            dropAfterMaxRetry[sid]++;
            sentCount[sid][buf->rIdx]--;
            xSemaphoreGive(statsMutex);
        }
        cleanupPacket(&buf->pkt);
        buf->retries = 0;
        xSemaphoreGive(sendBufferMutex[sid]);
        return;
    }
    if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) == pdTRUE) {
        retryCount[sid]++;
        sentCount[sid][buf->rIdx]++;
        xSemaphoreGive(statsMutex);}
    printf("[Sender %d] Retrying packet %lu | Attempt %d\n", sid, (unsigned long)buf->pkt->seqNumber, buf->retries);
    Packet *pktCopy = buf->pkt;
    if (xQueueSend(switchQueue, &pktCopy, QUEUE_WAIT_TICKS) != pdPASS) {
        printf("[Sender %d] Failed to resend packet to switch\n", sid);
    } else {
        xTimerStart(buf->timer, 0);
    }
    xSemaphoreGive(sendBufferMutex[sid]);}
void senderTask(void *param) {
    int sid = (int)(intptr_t)param;
    if (xSemaphoreTake(sendBufferMutex[sid], portMAX_DELAY) != pdTRUE) {
        vTaskDelete(NULL);
        return;
    }
    sendBuffers[sid].pkt = NULL;
    sendBuffers[sid].timer = xTimerCreate(
        "TOUT", pdMS_TO_TICKS(TOUT_MS), pdFALSE,
        (void *)(intptr_t)sid, senderTimeoutCallback
    );
    if (!sendBuffers[sid].timer) {
        printf("[Sender %d] Failed to create timer\n", sid);
        xSemaphoreGive(sendBufferMutex[sid]);
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(sendBufferMutex[sid]);
    while (1) {
        if (simulationComplete) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue; }
        if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (sendBuffers[sid].pkt) {
            xSemaphoreGive(sendBufferMutex[sid]);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;}
        xSemaphoreGive(sendBufferMutex[sid]);
        int delay = T1_MS + rand() % (T2_MS - T1_MS + 1);
        vTaskDelay(pdMS_TO_TICKS(delay));
        int destIdx = rand() % NUM_RECEIVERS;
        int dest = RECEIVER_ID_BASE + destIdx;
        uint16_t len = L1 + rand() % (L2 - L1 + 1);
        Packet *pkt = allocPacket();
        if (!pkt) {
            printf("[Sender %d] Packet allocation failed\n", sid);
            vTaskDelay(pdMS_TO_TICKS(10)); // Brief delay to avoid rapid retry
            continue;
        }
        pkt->senderId = sid;
        pkt->destination = dest;
        pkt->length = len;
        if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) == pdTRUE) {
            pkt->seqNumber = seqTracker[sid][destIdx]++;
            sentCount[sid][destIdx]++;
            printf("[Sender %d] Sending packet %u to Receiver %d\n",
                   sid, sentCount[sid][destIdx]-1, dest);
            xSemaphoreGive(statsMutex);
        } else {
            freePacket(pkt);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) {
            freePacket(pkt);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;}
        sendBuffers[sid].pkt = pkt;
        sendBuffers[sid].retries = 0;
        sendBuffers[sid].rIdx = destIdx;
        Packet *pktPtr = pkt;
        if (xQueueSend(switchQueue, &pktPtr, QUEUE_WAIT_TICKS) != pdPASS) {
            printf("[Sender %d] Failed to send to switch queue\n", sid);
            cleanupPacket(&sendBuffers[sid].pkt);
            sendBuffers[sid].retries = 0;
            xSemaphoreGive(sendBufferMutex[sid]);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (xTimerStart(sendBuffers[sid].timer, 0) != pdPASS) {
            printf("[Sender %d] Failed to start timer\n", sid);
            cleanupPacket(&sendBuffers[sid].pkt);
            sendBuffers[sid].retries = 0;
            xSemaphoreGive(sendBufferMutex[sid]);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        xSemaphoreGive(sendBufferMutex[sid]);}}
void ackReceiverTask(void *param) {
    int sid = (int)(intptr_t)param;
    AckPacket ack;
    while (1) {
        if (xQueueReceive(ackQueues[sid], &ack, portMAX_DELAY) == pdPASS) {
            if (xSemaphoreTake(sendBufferMutex[sid], MUTEX_WAIT_TICKS) != pdTRUE) {
                continue;
            }
            SendBuffer *buf = &sendBuffers[sid];
            if (buf->pkt && ack.seqNumber == buf->pkt->seqNumber) {
                xTimerStop(buf->timer, 0);
                cleanupPacket(&buf->pkt);
                buf->retries = 0;
            }
            xSemaphoreGive(sendBufferMutex[sid]);  }}}
void switchTask(void *param) {
    Packet *pkt;
    while (1) {
        if (xQueueReceive(switchQueue, &pkt, portMAX_DELAY) == pdPASS) {
            int drop = rand() % 1000000;
            if (drop < P_DROP) {
                printf("[Switch] Dropped packet %lu to R%d\n",
                       (unsigned long)pkt->seqNumber, pkt->destination);
                if (xSemaphoreTake(statsMutex, MUTEX_WAIT_TICKS) == pdTRUE) {
                    dropBySwitch[pkt->senderId]++;
                    xSemaphoreGive(statsMutex);
                }
                cleanupPacket(&pkt);
                continue;}
            uint32_t t_delay = (pkt->length * 8 * 1000) / LINK_CAPACITY;
            vTaskDelay(pdMS_TO_TICKS(PROPAGATION_DELAY + t_delay));
            int rIdx = pkt->destination - RECEIVER_ID_BASE;
            if (rIdx >= 0 && rIdx < NUM_RECEIVERS) {
                if (xQueueSend(receiverQueues[rIdx], &pkt, QUEUE_WAIT_TICKS) != pdPASS) {
                    printf("[Switch] Failed to send to receiver %d\n", pkt->destination);
                    cleanupPacket(&pkt);
                }
            } else {
                printf("[Switch] Invalid receiver index %d\n", rIdx);
                cleanupPacket(&pkt);} } }}
void receiverTask(void *param) {
    int rIdx = (int)(intptr_t)param;
    int rid = rIdx + RECEIVER_ID_BASE;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(receiverQueues[rIdx], &pkt, portMAX_DELAY) == pdPASS) {
            int sid = pkt->senderId;
            uint32_t seqNum = pkt->seqNumber;
            if (xSemaphoreTake(recvCountMutex, MUTEX_WAIT_TICKS) == pdTRUE) {
                recvCount[sid][rIdx]++;
                globalReceived++;
                recvCountPerReceiver[rIdx]++;
                totalBytesReceived += pkt->length;
                printf("[Receiver %d] Received packet %lu from S%d | Total: %u\n",
                       rid, (unsigned long)pkt->seqNumber, sid, globalReceived);
                xSemaphoreGive(recvCountMutex);
            }
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
                if (xQueueSend(ackQueues[ack.ackDest], &ack, QUEUE_WAIT_TICKS) != pdPASS) {
                    printf("[Receiver %d] Failed to send ACK\n", rid);
                }
            } else {
                printf("[Receiver %d] Dropped ACK for S%d | Seq %lu\n", rid, sid, (unsigned long)seqNum);
            }
            if (allReceiversReachedMin() && !simulationComplete) {
                simulationComplete = 1;
                xSemaphoreGive(simulationDoneSemaphore);  } } }}
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
            if (sendBuffers[i].timer) {
                xTimerStop(sendBuffers[i].timer, 0);
                xTimerDelete(sendBuffers[i].timer, 0);
                sendBuffers[i].timer = NULL;
            }
            cleanupPacket(&sendBuffers[i].pkt);
            xSemaphoreGive(sendBufferMutex[i]);
        }
        vTaskSuspend(senderTaskHandles[i]);
        vTaskSuspend(ackReceiverTaskHandles[i]);
    }
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); }}
int main(void) {
    srand(12345);
    initPacketPool();
    switchQueue = xQueueCreate(20, sizeof(Packet *));
    if (!switchQueue) {
        printf("Failed to create switch queue\n");
        return -1;
    }
    for (int i = 0; i < NUM_RECEIVERS; i++) {
        receiverQueues[i] = xQueueCreate(20, sizeof(Packet *));
        if (!receiverQueues[i]) {
            printf("Failed to create receiver queue %d\n", i);
            return -1;
        }
    }
    for (int i = 0; i < NUM_SENDERS; i++) {
        ackQueues[i] = xQueueCreate(20, sizeof(AckPacket));
        if (!ackQueues[i]) {
            printf("Failed to create ack queue %d\n", i);
            return -1;
        }
    }
    recvCountMutex = xSemaphoreCreateMutex();
    simulationDoneSemaphore = xSemaphoreCreateBinary();
    statsMutex = xSemaphoreCreateMutex();
    if (!recvCountMutex || !simulationDoneSemaphore || !statsMutex) {
        printf("Failed to create semaphores\n");
        return -1;
    }
    for (int i = 0; i < NUM_SENDERS; i++) {
        sendBufferMutex[i] = xSemaphoreCreateMutex();
        if (!sendBufferMutex[i]) {
            printf("Failed to create send buffer mutex %d\n", i);
            return -1;
        }
    }
    if (xTaskCreate(switchTask, "Switch", 2048, NULL, 3, NULL) != pdPASS) {
        printf("Failed to create switch task\n");
        return -1;
    }
    for (int i = 0; i < NUM_RECEIVERS; i++) {
        if (xTaskCreate(receiverTask, "Receiver", 2048, (void *)(intptr_t)i, 1, NULL) != pdPASS) {
            printf("Failed to create receiver task %d\n", i);
            return -1;
        }
    }
    for (int i = 0; i < NUM_SENDERS; i++) {
        if (xTaskCreate(senderTask, "Sender", 2048, (void *)(intptr_t)i, 2, &senderTaskHandles[i]) != pdPASS) {
            printf("Failed to create sender task %d\n", i);
            return -1;
        }
        if (xTaskCreate(ackReceiverTask, "AckRecv", 1024, (void *)(intptr_t)i, 2, &ackReceiverTaskHandles[i]) != pdPASS) {
            printf("Failed to create ack receiver task %d\n", i);
            return -1;
        }
    }
    if (xTaskCreate(simulationMonitorTask, "SimMonitor", 2048, NULL, 4, NULL) != pdPASS) {
        printf("Failed to create monitor task\n");
        return -1;
    }
    puts("Starting Send & Wait simulation...");
    simulationStartTime = xTaskGetTickCount();
    vTaskStartScheduler();
    while (1);
}
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
