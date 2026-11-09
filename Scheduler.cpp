//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <bits/stdc++.h>

static Scheduler scheduler_instance;

unsigned periodic_checks = 0;
static unsigned active_machines;
std::unordered_map<TaskId_t, MachineId_t> task_to_machine;
std::unordered_map<TaskId_t, VMId_t> task_to_vm;
std::vector<VMId_t> migrating_vms;
std::vector<MachineId_t> migrating_machines;

constexpr VMId_t INVALID_VM_ID = std::numeric_limits<VMId_t>::max();
constexpr MachineId_t INVALID_MACHINE_ID = std::numeric_limits<MachineId_t>::max();


unsigned algo = 3; // 1: first-fit, 2: best-fit, 3: pegasus, 4: dynamic allocation
void first_fit(TaskId_t task_id, Priority_t priority);
void best_fit(TaskId_t task_id, Priority_t priority);
void pegasus_new_task(TaskId_t task_id, Priority_t priority);
MachineId_t migrateTo(TaskId_t task_id);
void pegasus_sla_violation(TaskId_t task_id);
using SchedulerFunction = void (*)(TaskId_t, Priority_t);
SchedulerFunction funcs[] = {first_fit, best_fit, pegasus_new_task};


bool is_migrating(VMId_t vm_id) {
    return std::find(migrating_vms.begin(), migrating_vms.end(), vm_id) != migrating_vms.end();
}

VMId_t Scheduler::FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, MachineId_t machine_id) {
    for (auto & vm: Scheduler::vms) {
        VMInfo_t vm_info = VM_GetInfo(vm);
        if (vm_info.cpu == cpu_type && vm_info.vm_type == vm_type
                && vm_info.machine_id == machine_id && !is_migrating(vm)) {
            return vm;
        }
    }
    VMId_t vm_id = VM_Create(vm_type, cpu_type);
    Scheduler::vms.push_back(vm_id);
    VM_Attach(vm_id, machine_id);
    return vm_id;
}

void Scheduler::SLAWarning_Handler(Time_t time, TaskId_t task_id) {
    MachineId_t machine_id = task_to_machine[task_id];
    VMId_t vm_id = task_to_vm[task_id];
    SetTaskPriority(task_id, HIGH_PRIORITY);
    if (algo == 3) {
        pegasus_sla_violation(task_id);
        return;
    }
    VMId_t vm_neighbor = INVALID_VM_ID;
    MachineId_t best_machine = migrateTo(task_id);
    for (auto & vm: vms) {
        VMInfo_t vm_info = VM_GetInfo(vm);
        if (vm_info.machine_id == machine_id && vm_info.vm_id != vm_id) {
            vm_neighbor = vm_info.vm_id;
            break;
        }
    }
    if (vm_neighbor == INVALID_VM_ID) {
        return;
    }

    if (best_machine != INVALID_MACHINE_ID) {
        VM_Migrate(vm_neighbor, best_machine);
        task_to_machine[task_id] = best_machine;
        migrating_vms.push_back(vm_neighbor);
    } else {
        for (MachineId_t i = 0; i < Machine_GetTotal(); i++) {
            MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
            if (machine_info.s_state == S5 && machine_info.cpu == GetTaskInfo(task_id).required_cpu) {
                Machine_SetState(MachineId_t(i), S0);
                VM_Migrate(vm_neighbor, MachineId_t(i));
                task_to_machine[task_id] = MachineId_t(i);
                migrating_vms.push_back(vm_neighbor);
                return;
            }
        }
    }

}

//Algo1: first-fit
void first_fit(TaskId_t task_id, Priority_t priority) {
    TaskInfo_t task = GetTaskInfo(task_id);
    unsigned machines = Machine_GetTotal();

    for (MachineId_t i = 0; i < machines; i++) {
        MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
        if (machine_info.cpu == task.required_cpu
                && machine_info.memory_size >= machine_info.memory_used
                && machine_info.memory_size - machine_info.memory_used >= task.required_memory
                && machine_info.gpus >= task.gpu_capable
                && machine_info.s_state != S5) {
            VMId_t vm_id = scheduler_instance.FindOrCreateVM(
                task.required_cpu, task.required_vm, MachineId_t(i));
            if (vm_id == INVALID_VM_ID) return;
            task_to_machine[task_id] = MachineId_t(i);
            task_to_vm[task_id] = vm_id;
            VM_AddTask(vm_id, task_id, priority);
            return;
        }
    }
}

//Algo2: best-fit
void best_fit(TaskId_t task_id, Priority_t priority) {
    TaskInfo_t task = GetTaskInfo(task_id);
    unsigned machines = Machine_GetTotal();
    MachineId_t best_machine = INVALID_MACHINE_ID;
    double min_cpu_diff = 1.0;

    for (MachineId_t i = 0; i < machines; i++) {
        MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
        if (machine_info.cpu == task.required_cpu
                && machine_info.memory_size >= machine_info.memory_used
                && machine_info.memory_size - machine_info.memory_used >= task.required_memory
                && machine_info.gpus >= task.gpu_capable
                && machine_info.s_state != S5) {
            double cpu_diff = 1 - (static_cast<double>(machine_info.active_tasks) 
            / machine_info.num_cpus);
            if (best_machine == INVALID_MACHINE_ID || cpu_diff < min_cpu_diff) {
                min_cpu_diff = cpu_diff;
                best_machine = i;
            }
        }
    }

    if (best_machine != INVALID_MACHINE_ID) {
        VMId_t vm_id = scheduler_instance.FindOrCreateVM(
            task.required_cpu, task.required_vm, best_machine);
        if (vm_id == INVALID_VM_ID) return;
        task_to_machine[task_id] = MachineId_t(best_machine);
        task_to_vm[task_id] = vm_id;
        VM_AddTask(vm_id, task_id, priority);
    }
}

//Algo3: pegasus
//Check if less periodics work better than each time
void pegasus_new_task(TaskId_t task_id, Priority_t priority) {
    TaskInfo_t task = GetTaskInfo(task_id);
    unsigned machines = Machine_GetTotal();
    MachineId_t best_machine = INVALID_MACHINE_ID;
    double min_cpu_util = 1.0;

    for (MachineId_t i = 0; i < machines; i++) {
        MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
        if (machine_info.cpu == task.required_cpu
                && machine_info.memory_size >= machine_info.memory_used
                && machine_info.memory_size - machine_info.memory_used >= task.required_memory
                && machine_info.gpus >= task.gpu_capable
                && machine_info.s_state != S5) {
            double cpu_util = (static_cast<double>(machine_info.active_tasks) 
                / machine_info.num_cpus);
            if (cpu_util < min_cpu_util) {
                min_cpu_util = cpu_util;
                best_machine = i;
            }
        }
    }

    if (best_machine != INVALID_MACHINE_ID) {
        VMId_t vm_id = scheduler_instance.FindOrCreateVM(
            task.required_cpu, task.required_vm, best_machine);
        if (vm_id == INVALID_VM_ID) return;
        task_to_machine[task_id] = MachineId_t(best_machine);
        task_to_vm[task_id] = vm_id;
        VM_AddTask(vm_id, task_id, priority);
    }

}

void Scheduler::pegasus_periodic() {
    unsigned machines = Machine_GetTotal();
    for (MachineId_t i = 0; i < machines; i++) {
        MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
        for (auto & vm: vms) {
            VMInfo_t vm_info = VM_GetInfo(vm);
            if (vm_info.machine_id == MachineId_t(i)) {
                // Check the tasks assigned to this VM
                for (auto & task_id: vm_info.active_tasks) {
                    TaskInfo_t task_info = GetTaskInfo(task_id);
                    // Check if the task is approaching its SLA deadline
                    double instruction_ratio = static_cast<double>(task_info.remaining_instructions) 
                            / task_info.total_instructions;
                    double completion_ratio = (static_cast<double>(Now()) - task_info.arrival) 
                            / (task_info.target_completion - task_info.arrival);
                    if ((1 - instruction_ratio) > (1 - completion_ratio * (1.35))) {
                        Machine_SetCorePerformance(MachineId_t(i), 0, P0);
                    } else if ((1 - instruction_ratio) > (1 - completion_ratio)) {
                        if (machine_info.p_state != P0) {
                            Machine_SetCorePerformance(
                                MachineId_t(i), 0,
                                static_cast<CPUPerformance_t>(machine_info.p_state - 1));
                        }
                    } else if ((1 - instruction_ratio) < (1 - completion_ratio * (0.60))) {
                        if (machine_info.p_state != P3) {
                            Machine_SetCorePerformance(
                                MachineId_t(i), 0,
                                static_cast<CPUPerformance_t>(machine_info.p_state + 1));
                        }
                    }

                }
            }
        }
        // unsigned active_tasks = machine_info.active_tasks;
        // unsigned active_vms = machine_info.active_vms;
        // if (active_vms == 0) {
        //     continue; // Avoid division by zero
        // }
        // double instruction_ratio = static_cast<double>(active_tasks) / active_vms;
        // if (instruction_ratio > 1.0) {
        //     // Consider migrating VMs or adjusting resources
        //     SimOutput("Pegasus: High instruction ratio on machine " + to_string(i) + ": " + to_string(instruction_ratio), 2);
        // }
    }
}
void pegasus_sla_violation(TaskId_t task_id) {
    Machine_SetCorePerformance(task_to_machine[task_id], 0, P0);
}
//Algo4: dynamic consolidation
void sort_vms() {

}
void pabfd() {
}

void Scheduler::Init() {
    // Find the parameters of the clusters
    // Get the total number of machines
    // For each machine:
    //      Get the type of the machine
    //      Get the memory of the machine
    //      Get the number of CPUs
    //      Get if there is a GPU or not
    // 
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);
    active_machines = Machine_GetTotal();
    for (unsigned i = 0; i < active_machines; i++) {
        MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
    }

    // bool dynamic = false;
    // if(dynamic)
    //     for(unsigned i = 0; i<4 ; i++)
    //         for(unsigned j = 0; j < 8; j++)
    //             Machine_SetCorePerformance(MachineId_t(0), j, P3);
    // Turn off the ARM machines
    // for(unsigned i = 24; i < Machine_GetTotal(); i++)
    //     Machine_SetState(MachineId_t(i), S5);

    SimOutput("Scheduler::Init(): Initialized with " + to_string(vms.size()) + " VMs", 3);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
    migrating_vms.erase(std::remove(migrating_vms.begin(), migrating_vms.end(), vm_id), migrating_vms.end());
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Get the task parameters
    //  IsGPUCapable(task_id);
    //  GetMemory(task_id);
    //  RequiredVMType(task_id);
    //  RequiredSLA(task_id);
    //  RequiredCPUType(task_id);
    // Decide to attach the task to an existing VM, 
    //      vm.AddTask(taskid, Priority_T priority); or
    // Create a new VM, attach the VM to a machine
    //      VM vm(type of the VM)
    //      vm.Attach(machine_id);
    //      vm.AddTask(taskid, Priority_t priority) or
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    //
    // Turn on a machine, migrate an existing VM from a loaded machine....
    //
    // Other possibilities as desired
    SLAType_t required_sla = RequiredSLA(task_id);
    Priority_t priority;
    if (required_sla == SLA0) {
        priority = HIGH_PRIORITY;
    } else if(required_sla == SLA1 || required_sla == SLA2) {
        priority = MID_PRIORITY;
    } else {
        priority = LOW_PRIORITY;
    }
    funcs[algo-1](task_id, priority);
    // Skeleton code, you need to change it according to your algorithm
}

void Scheduler::PeriodicCheck(Time_t now) {
    // This method should be called from SchedulerCheck()
    // SchedulerCheck is called periodically by the simulator to allow you to monitor, make decisions, adjustments, etc.
    // Unlike the other invocations of the scheduler, this one doesn't report any specific event
    // Recommendation: Take advantage of this function to do some monitoring and adjustments as necessary
    if (algo == 3) {
        pegasus_periodic();
    } else {

    }
}

void Scheduler::Shutdown(Time_t time) {
    // Do your final reporting and bookkeeping here.
    // Report about the total energy consumed
    // Report about the SLA compliance
    // Shutdown everything to be tidy :-)
    for(auto & vm: vms) {
        VM_Shutdown(vm);
    }
    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    // Do any bookkeeping necessary for the data structures
    VMInfo_t vm_info = VM_GetInfo(task_to_vm[task_id]);
    MachineId_t machine_id = task_to_machine[task_id];
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    if (machine_info.active_tasks == 0 && machine_info.active_vms == 0) {
        Machine_SetState(machine_id, S0i1);
    }
    task_to_machine.erase(task_id);
    task_to_vm.erase(task_id);
    if (vm_info.active_tasks.empty()) {
        VM_Shutdown(vm_info.vm_id);
        vms.erase(std::remove(vms.begin(), vms.end(), vm_info.vm_id), vms.end());
    }

    // Decide if a machine is to be turned off, slowed down, or VMs to be migrated according to your policy
    // This is an opportunity to make any adjustments to optimize performance/energy
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " is complete at " + to_string(now), 4);
}

// Public interface below

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    scheduler_instance.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    scheduler_instance.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    scheduler_instance.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    // The simulator is alerting you that machine identified by machine_id is overcommitted
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    // The function is called on to alert you that migration is complete
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    scheduler_instance.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    // This function is called periodically by the simulator, no specific event
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    scheduler_instance.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    // This function is called before the simulation terminates Add whatever you feel like.
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;     // SLA3 do not have SLA violation issues
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time)/1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    
    scheduler_instance.Shutdown(time);
}

MachineId_t migrateTo(TaskId_t task_id) {
    TaskInfo_t task = GetTaskInfo(task_id);
    unsigned machines = Machine_GetTotal();
    MachineId_t best_machine = INVALID_MACHINE_ID;
    double min_cpu_util = 1.0;

    for (MachineId_t i = 0; i < machines; i++) {
        MachineInfo_t machine_info = Machine_GetInfo(MachineId_t(i));
        if (machine_info.cpu == task.required_cpu
                && machine_info.memory_size >= machine_info.memory_used
                && machine_info.memory_size - machine_info.memory_used >= task.required_memory
                && machine_info.gpus >= task.gpu_capable
                && machine_info.s_state != S5) {
            double cpu_util = (static_cast<double>(machine_info.active_tasks) / machine_info.num_cpus);
            if (cpu_util < min_cpu_util) {
                min_cpu_util = cpu_util;
                best_machine = i;
            }
        }
    }
    return best_machine;
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    scheduler_instance.SLAWarning_Handler(time, task_id);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
    migrating_machines.erase(std::remove(migrating_machines.begin(), migrating_machines.end(), machine_id), migrating_machines.end());
}
