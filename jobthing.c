// jobthing.c
// Author: Rohith Palakirti
//
// Usage: ./jobthing [-v] [-i inputfile] jobfile

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <csse2310a3.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#define READ_END 0
#define WRITE_END 1
#define MAX_SIZE 500

/*
* Struct Definitions
*/

/* CmdArgs Struct
* -----------------------------------------------
* Structure to hold the arguments parsed from the commandline
* verboseFlag: flag to indicate if -v is specified
* inputFlag: flag to indicate if -i is specified
* jobFileFlag: flag to indicate if job file is specified
* jobFile: the name of the job file
* inputFile: the name of the input file
* mainInput: the fd of the input file
*/
typedef struct {
    bool verboseFlag, inputFileFlag, jobFileFlag;
    char jobFile[MAX_SIZE];
    char inputFile[MAX_SIZE];
    int mainInput;
} CmdArgs;

/* JobProps Struct
* -----------------------------------------------
* Structure to hold the properties of each job parsed from the job file
* jobID: ID of the job
* jobPipeIn[2], jobPipeOut[2]: fds for input/output pipes between child process
*    and jobthing
* 
* restartCount: number of times the job needs to be respawned
* status: status of the job as reported by waitpid
* jobCmd: the command and the supplied arguments for the job
* infiniteRestart: boolean to indicate if the job is respawned continuously 
    (numRestarts = 0)
* runnable: flag to indicate if the job is runnable
* ended: flag to indicate if the job has ended
* runs: number of times the job has run
* linesto: numbers of lines of input that have been sent to the job
*/
typedef struct {
    int jobID, jobPipeIn[2], jobPipeOut[2], jobInput, jobOutput, restartCount,
            status;
    char jobCmd[MAX_SIZE];
    bool infiniteRestart, runnable;
    bool ended;
    int runs, linesto;
} JobProps;

/*
 * Function Prototypes
 */
CmdArgs parse_command_line_args(int argc, char *argv[]);
void print_std_err(int value);
char *parse_inputfile_path(int argc, char *arg, bool flag);
char *parse_jobfile_path(int argc, char *arg, bool flag);
FILE *open_jobfile(char *filepath);
FILE *open_inputfile(char *filepath);
char *trim_whitespace(char *str);
int count_colons(char *line);
void child_handler(int s);
pid_t spawn_child(int *i, int *jobInputList, int *jobOutputList,
        char jobCmdList[50], int jobPipeIn[2], int jobPipeOut[2]);
void sig_handler(int signo);

// Global variable for signal handling
// signals [0][0] = jobCount, [0][1] = SIGHUP
// signals [i][0] = jobnum, [i][1] = numrestarts, [i][2] = linesto, 
//    where i > 0, i = job ID
int signals[100][3];

/* int main(int argc, char *argv[])
* -----------------------------------------------
* Main function for the jobthing program
*
* args: int argc, char *argv[]
* Returns: 0 on success
* Errors: exits with various exit codes {1, 2, 3, 99} on failure
*/ 
int main(int argc, char *argv[]) {
    // Signal handling
    for (int i = 0; i < 3; i++) {
        signals[0][i] = 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGPIPE, &sa, 0);
    sigaction(SIGINT, &sa, 0);

    CmdArgs args = parse_command_line_args(argc, argv);
    FILE *jobFile;
    jobFile = open_jobfile(args.jobFile);
    char *jobLine;
    int jobCount = 0;
    int invalidJobs = 0;
    int arraySize = 2;
    pid_t *pids = malloc(sizeof(pid_t));
    JobProps *jobList = (JobProps *) calloc(1, sizeof(JobProps));

    while ((jobLine = read_line(jobFile))) {
        jobLine = trim_whitespace(jobLine);
        if ((jobLine[0] == '#') || (strlen(jobLine) == 0)) {
            free(jobLine);
            continue;
        }
        int colonCount = count_colons(jobLine);
        if (colonCount != 3) {
            if (args.verboseFlag == true) {
                fprintf(stderr, "Error: invalid job specification: %s\n",
                        jobLine);
            }
            free(jobLine);
            continue;
        }
        char *copyJobLine = strdup(jobLine);
        char **jobSpecs = split_line(jobLine, ':');
        int numRestarts;
        if (jobSpecs[0] != NULL && (strcmp(jobSpecs[0], "") != 0)) {
            char *endptr;
            numRestarts = strtol(jobSpecs[0], &endptr, 10);
            if ((endptr == jobSpecs[0]) || (*endptr != '\0') 
                    || numRestarts < 0) {
                if (args.verboseFlag == true) {
                    fprintf(stderr, "Error: invalid job specification: %s\n",
                            copyJobLine);
                }
                free(jobLine);
                free(copyJobLine);
                free(jobSpecs);
                continue;
            }
        } else if ((strcmp(jobSpecs[0], "") != 0)) {
            numRestarts = 0;
        }
        char *jobInput = strdup(jobSpecs[1]);
        char *jobOutput = strdup(jobSpecs[2]);
        if (jobSpecs[3][0] != ' ') {
            int count;
            char *jobSpecsCopy = strdup(jobSpecs[3]);
            char **cmdArgs = split_space_not_quote(jobSpecs[3], &count);
            if (count == 0) {
                if (args.verboseFlag == true) {
                    fprintf(stderr, "Error: invalid job specification: %s\n",
                            copyJobLine);
                }
                free(jobLine);
                free(jobSpecsCopy);
                free(cmdArgs);
                free(jobInput);
                free(jobOutput);
                free(copyJobLine);
                free(jobSpecs);
                continue;
            }
            jobCount++;
            if (jobCount == arraySize - 1) {
                arraySize = arraySize * 2;
                jobList =
                        (JobProps *) realloc(jobList, 
                        sizeof(JobProps) * arraySize);
                pids = (pid_t *) realloc(pids, sizeof(pids) * arraySize);
            }
            jobList[jobCount].jobID = jobCount;
            jobList[jobCount].runnable = true;
            jobList[jobCount].ended = false;
            jobList[jobCount].restartCount = numRestarts;
            strcpy(jobList[jobCount].jobCmd, jobSpecsCopy);
            if (args.verboseFlag) {
                printf("Registering worker %d: ", jobCount);
                fflush(stdout);
                for (int i = 0; i < count; i++) {
                    printf("%s", cmdArgs[i]);
                    if (i < count - 1) {
                        printf(" ");
                    }
                    fflush(stdout);
                }
                printf("\n");
                fflush(stdout);
            }
            if (jobList[jobCount].restartCount == 0) {
                jobList[jobCount].infiniteRestart = true;
            } else {
                jobList[jobCount].infiniteRestart = false;
            }
            if (strlen(jobInput) != 0) {
                jobList[jobCount].jobInput = open(jobInput, O_RDONLY);
                if (jobList[jobCount].jobInput == -1) {
                    fprintf(stderr,
                            "Error: unable to open \"%s\" for reading\n",
                            jobInput);
                    jobList[jobCount].runnable = false;
                    invalidJobs++;
                    free(jobLine); 
                    free(jobSpecsCopy);
                    free(cmdArgs);
                    free(jobInput);
                    free(jobOutput);
                    free(copyJobLine);
                    free(jobSpecs);
                    continue;
                }
            } else {
                jobList[jobCount].jobInput = -2;
            }
            if (strlen(jobOutput) != 0) {
                jobList[jobCount].jobOutput =
                        open(jobOutput, O_WRONLY | O_CREAT | O_TRUNC,
                        S_IWUSR | S_IRUSR);
                if (jobList[jobCount].jobOutput == -1) {
                    fprintf(stderr,
                            "Error: unable to open \"%s\" for writing\n",
                            jobOutput);
                    jobList[jobCount].runnable = false;
                    invalidJobs++;
                    free(jobLine);   
                    free(jobSpecsCopy);
                    free(cmdArgs);
                    free(jobInput);
                    free(jobOutput);
                    free(copyJobLine);
                    free(jobSpecs);
                    continue;
                }
            } else {
                jobList[jobCount].jobOutput = -2;

            }
        } else {
            if (args.verboseFlag == true) {
                fprintf(stderr, "Error: invalid job specification: %s\n",
                        copyJobLine);
            }
            free(jobLine);
            free(jobInput);
            free(jobOutput);
            free(copyJobLine);
            free(jobSpecs);
            continue;
        }
    }
    free(jobLine);
    fclose(jobFile);

    signals[0][0] = jobCount;
    int viableWorkers = jobCount - invalidJobs;
    int jobStatuses[jobCount];
    for (int i = 1; i <= jobCount; i++) {
        jobStatuses[i] = 0;
        jobList[i].status = 0;
        jobList[i].runs = 0;
        signals[i][1] = 0;
        jobList[i].linesto = 0;
        signals[i][2] = 0;
    }
    for (int i = 1; i <= jobCount; i++) {
        if (jobList[i].runnable == true) {
            pids[i] =
                    spawn_child(&i, &jobList[i].jobInput, 
                    &jobList[i].jobOutput, jobList[i].jobCmd, 
                    jobList[i].jobPipeIn, jobList[i].jobPipeOut);
            jobList[i].runs++;
            signals[i][1] = jobList[i].runs;
            if (args.verboseFlag) {
                printf("Spawning worker %d\n", i);
                fflush(stdout);
            }
        }
    }
    sleep(1);

    while (viableWorkers > 0) {
        for (int i = 1; i <= jobCount; i++) {
            jobStatuses[i] = waitpid(pids[i], &jobList[i].status, WNOHANG);
            if ((jobList[i].runnable == true) && (jobList[i].ended == false)) {
                if (jobStatuses[i] == 0) {
                    jobList[i].ended = false;
                } else if ((jobStatuses[i] != 0)
                        && WIFEXITED(jobList[i].status)) {
                    printf("Job %d has terminated with exit code %d\n", i,
                            WEXITSTATUS(jobList[i].status));
                    fflush(stdout);
                    kill(pids[i], SIGKILL);
                    viableWorkers--;
                    jobList[i].ended = true;
                } else if (((jobStatuses[i] != 0))
                        && WIFSIGNALED(jobList[i].status)) {
                    printf("Job %d has terminated due to signal %d\n", i,
                            WTERMSIG(jobList[i].status));
                    fflush(stdout);
                    kill(pids[i], SIGKILL);
                    viableWorkers--;
                    jobList[i].ended = true;
                }
                if (jobList[i].ended) {
                    close(jobList[i].jobPipeIn[READ_END]);
                    close(jobList[i].jobPipeOut[WRITE_END]);
                    close(jobList[i].jobPipeIn[WRITE_END]);
                    close(jobList[i].jobPipeOut[READ_END]);
                }
                if ((jobList[i].runs < jobList[i].restartCount)
                        || jobList[i].infiniteRestart == true) {
                    if (jobList[i].ended == true) {
                        pids[i] =
                                spawn_child(&i, &jobList[i].jobInput,
                                &jobList[i].jobOutput,
                                jobList[i].jobCmd,
                                jobList[i].jobPipeIn,
                                jobList[i].jobPipeOut);
                        viableWorkers++;
                        jobList[i].ended = false;
                        jobList[i].runs++;
                        signals[i][1] = jobList[i].runs;
                        if (args.verboseFlag) {
                            fprintf(stderr, "Restarting worker %d\n", i);
                        }
                    }
                } else if (((jobList[i].runs) > (jobList[i].restartCount))
                        && (jobList[i].infiniteRestart == false)) {
                    jobList[i].runnable = false;
                    jobList[i].ended = true;
                }
            }
        }
        if (viableWorkers < 1) {
            int flag = 0;
            for (int j = 1; j <= jobCount; j++) {
                if (!jobList[j].ended) {
                    flag = 1;
                }
            }
            if (flag == 0) {
                fprintf(stderr, "No more viable workers, exiting\n");
                fflush(stderr);
                free(pids);
                free(jobList);
                exit(0);
            }
        }
        char *inputLine = read_line(stdin);
        if (inputLine == NULL) {
            free(inputLine);
            exit(0);
        }
        if (inputLine[0] == '*') {
            sleep(1);
            char *inputCopy = strdup(inputLine);
            char **inputSplit = split_line(inputCopy, ' ');
            char *command = strdup(inputSplit[0]);
            int num = -111;
            int signum = -111;
            bool invalidNum = false;
            bool invalidSigNum = false;
            int lengthNum = -111;
            int lengthSigNum;
            if (inputSplit[1]) {
                lengthNum = strlen(inputSplit[1]);
            } else {
                lengthNum = -111;
            }
            if (lengthNum != -111) {
                char *endptr;
                num = strtol(inputSplit[1], &endptr, 10);
                if ((endptr == inputSplit[1]) || (*endptr != '\0')) {
                    invalidNum = true;
                }
            }
            if (inputSplit[2]) {
                lengthSigNum = strlen(inputSplit[2]);
                if (lengthSigNum) {
                    char *endptr;
                    signum = strtol(inputSplit[2], &endptr, 10);
                    if ((endptr == inputSplit[2]) || (*endptr != '\0')) {
                        invalidSigNum = true;
                    }
                }
            }
            if (strcmp(command, "*signal") == 0) {
                if ((num == -111) || (signum == -111)) {
                    printf("Error: Incorrect number of arguments\n");
                    free(inputLine);
                    continue;
                } else {
                    if ((num > jobCount) || (num < 1)
                            || (jobList[num].ended == true)
                            || (invalidNum)) {
                        invalidNum = true;
                        printf("Error: Invalid job\n");
                        free(inputLine);
                        continue;
                    }
                    if ((signum < 1) || (signum > 31)
                            || (invalidSigNum)) {
                        invalidSigNum = true;
                        printf("Error: Invalid signal\n");
                        free(inputLine);
                        continue;
                    }
                    if (!(invalidNum) && !(invalidSigNum)) {
                        int killReturn = kill(pids[num], signum);
                        if (killReturn == -1) {
                            fprintf(stderr, "Kill error\n");
                        }
                        sleep(1);
                        free(inputLine);
                        continue;
                    }
                }
            } else if (strcmp(command, "*sleep") == 0) {
                if ((num == -111) || (signum != -111)) {
                    printf("Error: Incorrect number of arguments\n");
                    free(inputLine);
                    continue;
                } else {
                    if (num < 0 || invalidNum) {
                        printf("Error: Invalid duration\n");
                        free(inputLine);
                        continue;
                    } else {
                        usleep(num * 1000);
                        sleep(1);
                        free(inputLine);
                        continue;
                    }
                }
            } else {
                printf("Error: Bad command '%s'\n", command);
                free(inputLine);
                continue;
            }
        } else {
            for (int j = 1; j <= jobCount; j++) {
                if (!jobList[j].ended) {
                    close(jobList[j].jobPipeIn[READ_END]);
                    close(jobList[j].jobPipeOut[WRITE_END]);
                    write(jobList[j].jobPipeIn[WRITE_END], inputLine,
                          strlen(inputLine));
                    write(jobList[j].jobPipeIn[WRITE_END], "\n", strlen("\n"));
                    if (jobList[j].jobInput == -2) {
                        printf("%d<-'%s'\n", j, inputLine);
                        fflush(stdout);
                        jobList[j].linesto++;
                        signals[j][2] = jobList[j].linesto;
                    } else {
                        jobList[j].linesto = 0;
                        signals[j][2] = jobList[j].linesto;
                    }
                }
            }
        }

        sleep(1);
        for (int j = 1; j <= jobCount; j++) {
            if ((jobList[j].runnable == true) && (!jobList[j].ended)
                    && (jobList[j].jobOutput == -2)) {
                FILE *output = fdopen(jobList[j].jobPipeOut[READ_END], "r");
                char *outputLine = read_line(output);
                if (!(outputLine)) {
                    if (args.verboseFlag) {
                        fprintf(stderr, "Received EOF from job %d\n", j);
                    }
                } else if (jobList[j].jobOutput == -2) {
                    printf("%d->'%s'\n", j, outputLine);
                    fflush(stdout);
                } else {
                    free(outputLine);
                    continue;
                }
                free(outputLine);
            }
        }
        if (signals[0][0]) {
        }
        if (signals[0][1]) {
        }
        if (signals[0][2]) {
        }
        free(inputLine);
        continue;
    }
    if (viableWorkers < 1) {
        fprintf(stderr, "No more viable workers, exiting\n");
        free(jobList);
        free(pids);
        exit(0);
    }
    return 0;
}

/* void sig_handler(int signo)
* -----------------------------------------------
* This function is called when a signal is received
*
* args: signo - the signal number 
*/
void sig_handler(int signo) {
    if (signo == SIGPIPE) {
    }
    if (signo == SIGHUP) {
        signals[0][1] = 1;
        for (int i = 1; i <= signals[0][0]; i++) {
            fprintf(stderr, "%d:%d:%d\n", i, signals[i][1], signals[i][2]);
        }
    }
    if (signo == SIGINT) {
        signals[0][2] = 1;
    }
}

/* pid_t spawn_child(int *i, int *jobInputList, int *jobOutputList,
        char jobCmdList[50], int jobPipeIn[2], int jobPipeOut[2])
* -----------------------------------------------
* This function spawns a child process
*
* args: i - the job number, jobInputList - the input list, jobOutputList - the 
* Returns: the pid of the spawned child
* Errors: exits with error code 0 if fork fails, exits with error code 99 if
     execvp fails
*/
pid_t spawn_child(int *i, int *jobInputList, int *jobOutputList,
        char jobCmdList[50], int jobPipeIn[2], int jobPipeOut[2]) {
    if (*jobInputList == -2) {
        // Create Pipe to redirect stdout from job to jobthing
        if (pipe(jobPipeIn) == -1) {
            perror("in");
        }
    }
    if (*jobOutputList == -2) {
        // Create Pipe to redirect stdin from jobthing to job
        if (pipe(jobPipeOut) == -1) {
            perror("out");
        }
    }

    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork() failed!\n");
        exit(0);
    }
    if (!pid) {
        if (*jobInputList == -2) {
            dup2(jobPipeIn[READ_END], STDIN_FILENO);
            close(jobPipeIn[WRITE_END]);
            close(jobPipeIn[READ_END]);
        } else {
            dup2(*jobInputList, STDIN_FILENO);
            close(*jobInputList);
        }
        if (*jobOutputList == -2) {
            dup2(jobPipeOut[WRITE_END], STDOUT_FILENO);
            close(jobPipeOut[READ_END]);
            close(jobPipeOut[WRITE_END]);
        } else {
            dup2(*jobOutputList, STDOUT_FILENO);
            close(*jobOutputList);
        }
        int cmdCount;
        char **jobCmdArgs = split_space_not_quote(jobCmdList, &cmdCount);
        char *jobArgs[cmdCount];
        for (int i = 0; i < cmdCount; i++) {
            jobArgs[i] = jobCmdArgs[i];
        }
        jobArgs[cmdCount] = 0;
        int exitStatus = execvp(jobArgs[0], jobArgs);
        fflush(stdout);
        if (exitStatus == -1) {
            _exit(99);
        }
    }
    return pid;
}

/* int count_colons(char *line)
* -----------------------------------------------
* Counts the number of colons found in a line
*
* args: a line of characters
* Returns: the count of colons 
*/
int count_colons(char *line) {
    int count = 0;
    for (int i = 0; line[i]; i++) {
        if (line[i] == ':') {
            count++;
        }
    }
    return count;
}

/* char *trim_whitespace(char *str)
* -----------------------------------------------
* Trims whitespace from the beginning and ending of a string
*
* args: string from which whitespace is to be trimmed
* Returns: the trimmed string 
*/
char *trim_whitespace(char *str) {
// Reference: https://stackoverflow.com/questions/122616
// /how-do-i-trim-leading-trailing-whitespace-in-a-standard-way
    char *end;
    // Trim leading space
    while (isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == 0) {            // All spaces?
        return str;
    }
    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    // Write new null terminator character
    end[1] = '\0';
    return str;
}

/* FILE *open_jobfile(char *filepath)
* -----------------------------------------------
* Opens the jobfile as a FILE* object
*
* args: the filepath of the jobfile to be opened
* Returns: The FILE* object jobfile
* Errors: exits with code 2 if the job file cannot be opened
*/
FILE *open_jobfile(char *filepath) {
    FILE *jobFile;
    jobFile = fopen(filepath, "r");
    if (jobFile == 0) {
        fprintf(stderr, "Error: Unable to read job file\n");
        exit(2);
    }
    return jobFile;
}

/* FILE *open_inputfile(char *filepath)
* -----------------------------------------------
* Opens the inputfile as a FILE* object
*
* args: the filepath of the inputfile to be opened
* Returns: The FILE* object inputfile
* Errors: exits with code 3 if the input file cannot be opened
*/
FILE *open_inputfile(char *filepath) {
    FILE *inputFile;
    inputFile = fopen(filepath, "r");
    if (inputFile == 0) {
        fprintf(stderr, "Error: Unable to read input file\n");
        exit(3);
    }
    return inputFile;
}

/* CmdArgs parse_command_line_args(int argc, char *argv[])
* -----------------------------------------------
* Parses arguments supplied through the command line and stores in a struct
*
* args: arg count and the commandline argument list
* Returns: The parsed arguments in CmdArgs struct
* Errors: exits with code 1 if the number of arguments is less than the minimum
    required
    exits with code 3 if the input file cannot be opened
*/
CmdArgs parse_command_line_args(int argc, char *argv[]) {
    if (argc < 2 || argc > 5) {
        print_std_err(1);
    }
    CmdArgs args;
    args.verboseFlag = args.inputFileFlag = args.jobFileFlag = 0;
    strcpy(args.jobFile, "");
    strcpy(args.inputFile, "");
    for (int i = 1; i < argc; i++) {
        if (args.verboseFlag && args.inputFileFlag && argc < 5) {
            print_std_err(1);
        }
        if (argv[i][0] != '-' && (argv[i - 1][0] != '-'
                || (strcmp(argv[i - 1], "-v") == 0))) {
            if (!args.jobFileFlag) {
                char *jobFile = parse_jobfile_path(argc, argv[i],
                                                   args.jobFileFlag);
                strcpy(args.jobFile, jobFile);
                args.jobFileFlag = true;
            } else {
                print_std_err(1);
            }
        }
    }
    for (int i = 1; i < argc; i++) { 
        if (argv[i][0] == '-') {
            if ((strcmp(argv[i], "-i") == 0)) {
                if (argc < 4 || args.inputFileFlag) {
                    print_std_err(1);
                } else if (args.verboseFlag && argc < 5) {
                    print_std_err(1);
                }
                char *inputFile = parse_inputfile_path(argc, argv[i + 1],
                                                       args.inputFileFlag);
                if (strlen(inputFile) > 0) {
                    strcpy(args.inputFile, inputFile);
                    args.inputFileFlag = true;
                }
                if (args.inputFileFlag) {
                    args.mainInput = open(args.inputFile, O_RDONLY);
                    if (args.mainInput == -1) {
                        fprintf(stderr, "Error: Unable to read input file\n");
                        exit(3);
                    } else {
                        dup2(args.mainInput, STDIN_FILENO);
                        close(args.mainInput);
                    }
                }
            } else if ((strcmp(argv[i], "-v") == 0)) {
                if (argc < 3 || args.verboseFlag) {
                    print_std_err(1);
                }
                args.verboseFlag = 1;
            } else {
                print_std_err(1);
            }
        }
    }
    if (!args.jobFileFlag || strcmp(args.jobFile, "") == 0) {
        print_std_err(1);
    }
    return args;
}

/* char *parse_inputfile_path(int argc, char *arg, bool flag)
* -----------------------------------------------
* Checks validity of the inputfile path argument and parses it
*
* args: argument count, argument to be parsed and the inputfile flag
* Returns: The verified inputfile path argument
* Errors: exits with code 1 if the path is invalid
*/
char *parse_inputfile_path(int argc, char *arg, bool flag) {
    if (!flag) {
        if (arg == 0 || strlen(arg) <= 0 || argc < 3) {
            print_std_err(1);
        }
    } else {
        print_std_err(1);
    }
    return arg;
}

/* char *parse_jobfile_path(int argc, char *arg, bool flag)
* -----------------------------------------------
* Checks validity of the jobfile path argument and parses it
*
* args: argument count, argument to be parsed and the jobfile flag
* Returns: The verified jobfile path argument
* Errors: exits with code 1 if the path is invalid
*/
char *parse_jobfile_path(int argc, char *arg, bool flag) {
    if (!flag) {
        if (arg == 0 || strlen(arg) <= 0 || argc < 2) {
            print_std_err(1);
        }
    } else {
        print_std_err(1);
    }
    return arg;
}

/* void print_std_err(int value)
* -----------------------------------------------
* Prints out the standard error message on invalid input &
* Exits with the supplied exit code.
*
* args: exit code
*/
void print_std_err(int value) {
    fprintf(stderr, "Usage: jobthing [-v] [-i inputfile] jobfile\n");
    exit(value);
}

/* void free_memory(char **array, int *count)
* -----------------------------------------------
* Free the memory allocated for array
*
* args: array: array that is to be freed
*       count: number of elements in the array
*/
void free_memory(char **array, int *count) {
    for (int i = 0; i < *count; i++) {
        free(array[i]);
    }
    free(array);
}
