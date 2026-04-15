# Multithreaded Process Manager Simulator

## Project Overview
This project simulates the behavior of a multithreaded process manager to demonstrate the management of multiple processes and the resource allocation to improve efficiency and performance.

## Features
- Simulates the creation and management of multithreaded processes.
- Resource allocation algorithms to manage CPU and memory.
- Graphical user interface to visualize processes and resources.
- Supports various scheduling algorithms including Round Robin, FCFS, and SJF.

## Architecture
The architecture consists of several components including:
- **Process Manager**: Manages the lifecycle of processes.
- **Resource Allocator**: Allocates CPU time and memory resources.
- **Scheduler**: Implements the scheduling algorithms.
- **User Interface**: Provides a visual representation of the processes.

## Building and Running Instructions
1. Clone the repository:
   ```bash
   git clone https://github.com/SaucySalamander1/Multithreaded-Process-Manager-Simulator-
   cd Multithreaded-Process-Manager-Simulator-
   ```
2. Build the project:
   ```bash
   make
   ```
3. Run the simulator:
   ```bash
   ./simulator
   ```

## File Structure
```
Multithreaded-Process-Manager-Simulator
├── src                # Source files
├── include             # Header files
├── docs               # Documentation
├── tests              # Test cases
├── README.md          # Project documentation
└── Makefile           # Build instructions
```

## Example Usage
1. Start the simulator.
2. Add processes using the user interface.
3. Select a scheduling algorithm and begin execution.
4. Monitor the process states through the graphical interface.