#include <esp_heap_caps.h>

void recordHeapCheckpoint(const char* tag) {
    if (heapCheckpointCount >= HEAP_CHECKPOINT_MAX) {
        return;
    }

    HeapCheckpoint& checkpoint = heapCheckpoints[heapCheckpointCount++];
    checkpoint.tag = tag;
    checkpoint.freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    checkpoint.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    checkpoint.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

void reserveHotStrings() {
    currentCommand.reserve(128);
    commandHistoryDraft.reserve(128);
    cwd.reserve(64);

    for (int i = 0; i < COMMAND_HISTORY_MAX; i++) {
        commandHistory[i].reserve(64);
    }
}

void handleFreeCommand(const String parts[], int partCount){
    if(partCount == 1){
        showFree();
    } else if(parts[1] == "details"){
        showFreeDetailed();
    } else{
        showFreeHelp();
    }

}
void showFree(){
    addWrappedHistoryLine("TOTAL FREE: " + String(ESP.getFreeHeap()), RED);
    addWrappedHistoryLine("MAXALLOCHEAP: " + String(ESP.getMaxAllocHeap()), RED);
    addWrappedHistoryLine("MINALLOCHEAP: " + String(ESP.getMinFreeHeap()), RED);
}

void showFreeDetailed(){
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);

    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    int fragPct = freeHeap == 0 ? 0 : 100 - (int)((largest * 100) / freeHeap);

    addWrappedHistoryLine("FREE: " + String(freeHeap));
    addWrappedHistoryLine("LARGEST: " + String(largest));
    addWrappedHistoryLine("MIN FREE: " + String(minFree));
    addWrappedHistoryLine("FRAG: " + String(fragPct) + "%");
    addWrappedHistoryLine("ALLOC COUNT: " + String(info.total_allocated_bytes));

    if (heapCheckpointCount > 0) {
        addWrappedHistoryLine("BOOT CHECKPOINTS:", CYAN);
        for (int i = 0; i < heapCheckpointCount; i++) {
            const HeapCheckpoint& checkpoint = heapCheckpoints[i];
            addWrappedHistoryLine(
                String(checkpoint.tag)
                + " free=" + String(checkpoint.freeHeap)
                + " largest=" + String(checkpoint.largestBlock)
                + " min=" + String(checkpoint.minFreeHeap),
                CYAN
            );
        }
    }
}

void showFreeHelp(){
    addWrappedHistoryLine("free: Shows free RAM", CYAN);
    addWrappedHistoryLine("free details: show details", CYAN);
}
