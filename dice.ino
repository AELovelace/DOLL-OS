//dice roller application
//for funsies and practice. 
//wrote from scratch
void diceHelp(){
    addWrappedHistoryLine("Syntax: Dice [numSides] [numRolls]", CYAN);
}
void handleDiceCommand(const String parts[], int partCount){
    int diceSides = 6;
    int diceNumber = 1;
    if(partCount == 1) {
        diceHelp();
        return;
    } else {
        if(parts[1].length() != 0){
            diceSides = parts[1].toInt();  
        }
        if(parts[2].length() != 0){
            diceNumber = parts[2].toInt();
        }
    }
    diceRoll(diceSides, diceNumber);
}
void diceRoll(int sides, int rolls){
    String result[rolls];
    int color = random(3);
    String resultPrint = "Your roll: ";
    for(int i = 0; i < rolls; i++){
        result[i] = random(sides) + 1;
        if(i < rolls-1){
            resultPrint += result[i] + ", ";
        } else {
            resultPrint += result[i];
        }
    }
    switch(color){
        case 0:
            addWrappedHistoryLine(resultPrint, CYAN);
            break;
        case 1:
            addWrappedHistoryLine(resultPrint, PINK);
            break;
        case 2:
            addWrappedHistoryLine(resultPrint, GREEN);
            break;
    }
}