#include "tinyexpr.h"

void handleCalcCommand(const String parts[], int partCount){

    if(partCount == 1){
        showCalcHelp(0);
    } else if(parts[1] == "help"){
        showCalcHelp(1);
    }
    else if(partCount == 2){
        int success = 1;
        const char* expression = parts[1].c_str();
        double expressionResult = te_interp(expression, &success);
        addWrappedHistoryLine(String(expression), PINK);
        addWrappedHistoryLine(String(expressionResult), CYAN);
    } else {
        showCalcHelp(2);
    }
    
}

void showCalcHelp(int helpMsg){
    int sizeOfMessage = 0;
    switch(helpMsg){
        case 0: sizeOfMessage = 2;break;
        case 1: sizeOfMessage = 8;break;
        case 2: sizeOfMessage = 2;break;
    }
    String calcHelpMsg[sizeOfMessage];
    uint16_t calcLineColor[sizeOfMessage];
    switch(helpMsg){
        case 0: 
            calcHelpMsg[0] = "Syntax: calc (expression)";       calcLineColor[0]=PINK;
            calcHelpMsg[1] = "calc help for details";           calcLineColor[1]=CYAN;
            break;
        case 1:
            calcHelpMsg[0] = "Supported Functions";             calcLineColor[0]=PINK;
            calcHelpMsg[1] = "BASIC: + - * / ^ %";              calcLineColor[1]=WHITE;
            calcHelpMsg[2] = "abs acos asin atan atan2 ceil";   calcLineColor[2]=WHITE;
            calcHelpMsg[3] = "cos cosh exp floor ln log";       calcLineColor[2]=WHITE;
            calcHelpMsg[4] = "log10 pow sin sinh sqrt tan";     calcLineColor[2]=WHITE;
            calcHelpMsg[5] = "tanh";                            calcLineColor[2]=WHITE;
            calcHelpMsg[6] = "SPACES NOT SUPPORTED";            calcLineColor[2]=CYAN;
            calcHelpMsg[7] = "POWERED BY codeplea/tinyexpr";    calcLineColor[2]=YELLOW;
            break;
        case 2:
            calcHelpMsg[0] = "only two arguments, no spaces";   calcLineColor[0]=RED;
            calcHelpMsg[1] = "Syntax: calc (expression)";       calcLineColor[1]=GREEN;
            break;
    }
    for (int i = 0; i < sizeOfMessage; i++){
        addWrappedHistoryLine(calcHelpMsg[i], calcLineColor[i]);
    }
}