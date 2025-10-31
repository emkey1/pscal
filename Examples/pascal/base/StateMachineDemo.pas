program StateMachineDemo;

// Define the interface for a state
type
    IState = interface
        procedure GetName(var name: string);
        procedure Enter;
        procedure Update; // Places the next state interface into a global slot when transitioning
    end;

    PWorkState = ^TWorkState;
    PRestState = ^TRestState;

// --- State 1: A "Work" state ---
    TWorkState = record
        workCounter: integer;
        // IState implementation
        procedure GetName(var name: string); virtual;
        procedure Enter; virtual;
        procedure Update; virtual;
    end;

// --- State 2: A "Rest" state ---
    TRestState = record
        restDuration: integer;
        // IState implementation
        procedure GetName(var name: string); virtual;
        procedure Enter; virtual;
        procedure Update; virtual;
    end;

var
    gTransitionPending: boolean;
    gNextState: IState;

procedure TWorkState.GetName(var name: string);
begin
    name := 'Working';
end;

procedure TWorkState.Enter;
begin
    writeln('Entering WORK state...');
    // 'myself' refers to this record instance
    myself.workCounter := 0;
end;

procedure TWorkState.Update;
var
    newState: PRestState; // Pointer for the new state
begin
    gTransitionPending := false;
    gNextState := nil; // Default: stay in this state
    myself.workCounter := myself.workCounter + 1;
    writeln('...work...work... (', myself.workCounter, ')');

    if (myself.workCounter > 2) then
    begin
        writeln('Tired. Transitioning to REST state.');
        new(newState); // Create the new state record
        newState^.restDuration := 3;
        gNextState := IState(newState); // Box the record pointer into the interface
        gTransitionPending := true;
    end;
end;

procedure TRestState.GetName(var name: string);
begin
    name := 'Resting';
end;

procedure TRestState.Enter;
begin
    writeln('Entering REST state for ', myself.restDuration, ' ticks.');
end;

procedure TRestState.Update;
var
    newState: PWorkState; // Pointer for the new state
begin
    gTransitionPending := false;
    gNextState := nil; // Default: stay in this state
    myself.restDuration := myself.restDuration - 1;
    writeln('...zzZ... (', myself.restDuration, ' ticks left)');

    if (myself.restDuration <= 0) then
    begin
        writeln('Rested. Transitioning to WORK state.');
        new(newState); // Create the new state record
        newState^.workCounter := 0;
        gNextState := IState(newState); // Box it
        gTransitionPending := true;
    end;
end;


// --- Main Program ---
var
    initialState: PWorkState;
    currentState: IState;
    nextState: IState;
    stateName: string;
    i: integer;
begin
    // Create and set the initial state
    new(initialState);
    currentState := IState(initialState);
    currentState.Enter;
    gTransitionPending := false;

    // Run the state machine loop
    for i := 1 to 10 do
    begin
        currentState.GetName(stateName);

        writeln('');
        writeln('--- Tick ', i, ' (Current State: ', stateName, ') ---');

        // Update populates nextState and signals when a transition should occur
        currentState.Update;

        if gTransitionPending then
        begin
            nextState := gNextState;
            currentState := nextState;
            currentState.Enter; // Run the Enter method on the new state
        end;
    end;

    writeln('');
    writeln('State machine demo finished.');
end.
