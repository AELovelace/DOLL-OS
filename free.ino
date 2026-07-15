#include <esp_heap_caps.h>

static void showStringState(const char* label, const String& value) {
    addWrappedHistoryLine(String(label)
    + " len=" + String(value.length()), CYAN);
}

static void showHeapCapsSummary(const char* label, uint32_t caps) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);

    size_t freeHeap = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    if (freeHeap == 0 && largest == 0 && info.total_allocated_bytes == 0) {
        return;
    }

    int fragPct = freeHeap == 0 ? 0 : 100 - (int)((largest * 100) / freeHeap);
    addWrappedHistoryLine(String(label)
        + " free=" + String(freeHeap)
        + " largest=" + String(largest)
        + " frag=" + String(fragPct) + "%"
        + " alloc=" + String(info.total_allocated_bytes), CYAN);
}

void recordHeapCheckpoint(const char* tag) {
    int slot;
    if (heapCheckpointCount < HEAP_CHECKPOINT_MAX) {
        slot = (heapCheckpointHead + heapCheckpointCount) % HEAP_CHECKPOINT_MAX;
        heapCheckpointCount++;
    } else {
        slot = heapCheckpointHead;
        heapCheckpointHead = (heapCheckpointHead + 1) % HEAP_CHECKPOINT_MAX;
    }

    HeapCheckpoint& checkpoint = heapCheckpoints[slot];
    checkpoint.tag = tag;
    checkpoint.freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    checkpoint.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    checkpoint.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

void reserveHotStrings() {
    currentCommand.reserve(128);
    commandHistoryDraft.reserve(128);
    cwd.reserve(64);
    sshInputBuffer.reserve(128);
    motokoChannel.reserve(64);
    motokoInputBuffer.reserve(256);
    telnetAnsi.csiParams.reserve(32);
    sshStdoutAnsi.csiParams.reserve(32);
    sshStderrAnsi.csiParams.reserve(32);
    telnetStream.pendingRow.reserve(HISTORY_ROW_MAX_CHARS - 1);
    sshStdoutStream.pendingRow.reserve(HISTORY_ROW_MAX_CHARS - 1);
    sshStderrStream.pendingRow.reserve(HISTORY_ROW_MAX_CHARS - 1);

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
    addWrappedHistoryLine("ALLOC BYTES: " + String(info.total_allocated_bytes));
    addWrappedHistoryLine("FREE BLOCKS: " + String(info.free_blocks));
    addWrappedHistoryLine("ALLOC BLOCKS: " + String(info.allocated_blocks));

    addWrappedHistoryLine("HEAP CAPS:", CYAN);
    showHeapCapsSummary("8BIT", MALLOC_CAP_8BIT);
    showHeapCapsSummary("32BIT", MALLOC_CAP_32BIT);

#if defined(BOARD_HAS_PSRAM)
    showHeapCapsSummary("SPIRAM", MALLOC_CAP_SPIRAM);
#endif

    addWrappedHistoryLine("STRING STATE:", CYAN);
    showStringState("currentCommand", currentCommand);
    showStringState("commandDraft", commandHistoryDraft);
    showStringState("cwd", cwd);
    showStringState("sshInput", sshInputBuffer);
    showStringState("motokoChannel", motokoChannel);
    showStringState("motokoInput", motokoInputBuffer);
    showStringState("telnetAnsi", telnetAnsi.csiParams);
    showStringState("sshStdoutAnsi", sshStdoutAnsi.csiParams);
    showStringState("sshStderrAnsi", sshStderrAnsi.csiParams);
    showStringState("telnetRow", telnetStream.pendingRow);
    showStringState("sshStdoutRow", sshStdoutStream.pendingRow);
    showStringState("sshStderrRow", sshStderrStream.pendingRow);
    addWrappedHistoryLine("commandHistory used=" + String(commandHistoryCount)
        + "/" + String(COMMAND_HISTORY_MAX), CYAN);

    if (heapCheckpointCount > 0) {
        addWrappedHistoryLine("HEAP CHECKPOINTS:", CYAN);
        for (int i = 0; i < heapCheckpointCount; i++) {
            const HeapCheckpoint& checkpoint = heapCheckpoints[(heapCheckpointHead + i) % HEAP_CHECKPOINT_MAX];
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
