#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAXARGS 512
#define MAXLEN 2048

volatile sig_atomic_t fg_only_mode = 0;

//to keep track of what's happenning in the shell
typedef struct s_shell {
    int last_status; //status of last foreground process
    int last_signal; //signal that caused last foreground process to terminate
    pid_t child_pids[MAXARGS]; //array to hold background process pids
    int num_children; //number of background processes currently running
} Shell;

//structure to hold a command, arguements, etc
typedef struct cmd {
    char **argv; //arguments
    int argc; //number of arguments
    char *input_file; 
    char *output_file;
    int background; //0 for foreground, 1 for background
    int status;
    struct cmd *next; //pointer to the next command in the pipeline
} cmd;

/***************************************************************************
 * built-in commands:
 * 1. cd
 * 2. exit_command
 * 3. status_command
 * 4. expand_pid
****************************************************************************/

void status_command(Shell *shell) {
    if (shell->last_signal != 0) {
        printf("terminated by signal %d\n", shell->last_signal);
    } else {
        printf("exit value %d\n", shell->last_status);
    }
}

void cd(char **argv){

    char *path;
    if (!argv[1]) {
        path = getenv("HOME"); //if no argument, change to home directory
    } else {
        path = argv[1];
    }

    if (chdir(path) != 0) {
        perror("cd"); //print error if chdir fails
    }
}

void exit_command(Shell *shell) {
    //kill any background processes
    for (int i = 0; i < shell->num_children; i++){
        kill(shell->child_pids[i], SIGTERM);
        waitpid(shell->child_pids[i], NULL, 0);
    }
}

void expand_pid (char *line, pid_t pid) {
    char buffer[MAXLEN];
    char pid_str[20]; //buffer to hold pid as string
    sprintf(pid_str, "%d", pid); //convert pid to string

    char *src = line; //pointer to current position in input line
    char *dst = buffer; //pointer to current position in output buffer

    while (*src) {
        if (*src == '$' && *(src + 1) == '$') {
            for (char *p = pid_str; *p; p++) {
                *dst++ = *p;
            }
 
            src += 2; //skip over "$$"
        } else {
            *dst++ = *src++; //copy current character to output buffer
        }
    }
    *dst = '\0'; //null terminate output buffer
    strcpy(line, buffer); //copy expanded line back to original input line
}

/***************************************************************************
 * signal handlers:
 * 1. handle_sigtstp
 * 2. setup_signals
 * 3. child_signals
 * 
****************************************************************************/

//will toggle foreground mode off and on
void handle_sigtstp(int signum) {
    
    char *msg_on = "\nEntering foreground only mode (& is now ignored)\n";
    char *msg_off = "\nExiting foreground only mode\n";

    if (fg_only_mode == 0) {
        fg_only_mode = 1;
        write(STDOUT_FILENO, msg_on, strlen(msg_on));
    } else {
        fg_only_mode = 0;
        write(STDOUT_FILENO, msg_off, strlen(msg_off));
    }
}

//for parent process to ignore SIGINT
void setup_signals(void) {
    
    //ignore SIGINT for parent
    struct sigaction sig_c = {0};
    sig_c.sa_handler = SIG_IGN; //ignore sigint
    sigfillset(&sig_c.sa_mask);
    sig_c.sa_flags = 0;
    sigaction(SIGINT, &sig_c, NULL);

    //handle SIGTSTP
    struct sigaction sig_z = {0};
    sig_z.sa_handler = handle_sigtstp;
    sigfillset(&sig_z.sa_mask);
    sig_z.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sig_z, NULL);
}

//for child processes to restore default SIGINT behavior
void child_signals(void) {
    
    //SIGINT
    struct sigaction sig_c = {0};
    sig_c.sa_handler = SIG_DFL; //default handler for sigint
    sigfillset(&sig_c.sa_mask);
    sig_c.sa_flags = 0;

    //SIGTSTP
    struct sigaction sig_z = {0};
    sig_z.sa_handler = SIG_IGN; //ignore sigtstp
    sigfillset(&sig_z.sa_mask);
    sig_z.sa_flags = 0;

    sigaction(SIGINT, &sig_c, NULL);
    sigaction(SIGTSTP, &sig_z, NULL);
}

/***************************************************************************
 * looping main functions::
 * 1. read_line
 * 2. build_cmds
 * 3. parse_line
 * 4. free_cmd
****************************************************************************/

//will read a line of input from the user and return it as a string
char * read_line() {
    char *line = malloc(MAXLEN);
    if (fgets(line, MAXLEN, stdin) == NULL) {
        free(line);
        return NULL;
    }
    return line;
}

//creates linked list of cmd structs from array of tokens, handling redirection and background processes
cmd * build_cmds(char **tokens, int ntokens) {
    cmd *current = calloc(1, sizeof(cmd)); 
    current->argv = calloc(MAXARGS, sizeof(char *)); 
    current->argc = 0; //initialize argc for first command

    if (ntokens > 0 && strcmp(tokens[ntokens -1], "&") == 0) {
            //if there is & at the end then it will be in the background
            current->background = 1;
            ntokens--; //can frop the & token once added to the command struct
    }

    for (int i = 0; i < ntokens; i++) {
        if (strcmp(tokens[i], "<") == 0) {
            //input redirection
            if ( i + 1 < ntokens) {
                current->input_file = strdup(tokens[++i]);
            }
        } else if (strcmp(tokens[i], ">") == 0) {
            //output redirection
            if (i + 1 < ntokens) {
                current->output_file = strdup(tokens[++i]);
            }
        } else {
            current->argv[current->argc++] = strdup(tokens[i]);
        }
    }
    
    current->argv[current->argc] = NULL; //last in the list is null
    
    return current;
}

//will parse the input line into tokens and build array
//then calls build cmd to return a linked list of cmd structs
cmd * parse_line(char *line){
    
    //check to make sure in bounds for arguments before parsing
    if (strlen(line) >= MAXLEN) {
        fprintf(stderr, "improper command input\n");
        return NULL;
    }

    if (line[0] == '#' || line[0] == '\n') {
        //comment or blank line, ignore
        return NULL;
    }

    expand_pid(line, getpid());

    char *tokens[MAXARGS]; //array to hold tokens from input line
    int ntokens = 0; //number of tokens parsed

    //create first token from input line
    char *token = strtok(line, " \t\n");

    //loop to tokenize input line and store tokens in array
    while (token != NULL && ntokens < MAXARGS) {
        tokens[ntokens++] = token;
        token = strtok(NULL, " \t\n");
    }


    cmd *c = build_cmds(tokens, ntokens); 
}



//execute another command by forking and execing, 
//also handle background processes and updating shell status
void exec_command(Shell *shell, cmd *commands) {


    //enforce fg only mode
    if (fg_only_mode ==1) {
        commands->background = 0;
    }

    pid_t child_pid = fork();

    //check for fork errror
    if (child_pid == -1) {
        perror( "fork");
        return;
    }
    
    //child process
    if (child_pid == 0) {
        child_signals(); //restore default SIGINT behavior in child
        
        //background redirection to /dev/null
        if (commands->background) {
            if(!commands->input_file) {
                int in = open("/dev/null", O_RDONLY);
                dup2(in, STDIN_FILENO);
                close(in);
            }
            if ( !commands->output_file) {
                int out = open("/dev/null", O_WRONLY);
                dup2(out, STDOUT_FILENO);
                close(out);
            }

            signal(SIGINT, SIG_IGN);
        }

        //input redirection
        if (commands->input_file) {
            int newfd = open(commands->input_file, O_RDONLY);
            
            //check for error opening file
            if (newfd == -1) {
                perror("open input file");
                exit(1);
            }

            dup2(newfd, STDIN_FILENO); //redirect stdin to newfd
            close(newfd); //close original fd
        }

        //output redirection
        if(commands->output_file) {
            int newfd = open(commands->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

            //check for error opening file
            if (newfd == -1) {
                perror("open output file");
                exit(1);
            }

            dup2(newfd, STDOUT_FILENO); //redirect stdout to newfd
            close(newfd); //close original fd
        }

        execvp(commands->argv[0], commands->argv); //execute command with execvp

        perror("execvp"); //if execvp returns, there was an error
        exit(1);

    } else {
        if (commands->background) {
            //background process, add to shell's child_pids array
            shell->child_pids[shell->num_children++] = child_pid;
            printf("background pid is %d\n", child_pid); 
        } else {
            int status;
            waitpid(child_pid, &status, 0); //wait for child to finish
            
            if (WIFEXITED(status)) {
                shell->last_status = WEXITSTATUS(status);
                shell->last_signal = 0;
            } else if (WIFSIGNALED(status)) {
                shell->last_status = 0;
                shell->last_signal = WTERMSIG(status);

                printf("terminated by signal %d\n", shell->last_signal);
                fflush(stdout);
            }
        }
    }
}

//free the memory allocated for a cmd struct and its linked list
void free_cmd(cmd *c) {
    while (c != NULL) {
        cmd *next = c->next;
        for (int i = 0; c->argv[i] != NULL; i++) {
            free(c->argv[i]);
        }
        free(c->argv);
        if (c->input_file) { 
            free(c->input_file);
        }
        if (c->output_file) {
            free(c->output_file);
        }
        free(c);
        c = next;
    }
}

int main (int argc, char *argv[]) {
    
    setup_signals(); //ignore SIGINT in the shell process
    Shell shell = {0};

    while (1) {
        int status;
        pid_t pid;

        //cleanup
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            printf("background pid %d is done: ", pid);

            if (WIFEXITED(status)) {
                printf("exit value %d\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("terminated by signal %d\n", WTERMSIG(status));
            }
            fflush(stdout);
        }

        printf(": ");
        fflush(stdout);

        char *line = read_line();


        if (line == NULL) {
            break;
        }

        cmd *commands = parse_line(line);

        //checking
        if (commands == NULL || commands->argv[0] == NULL){
            free(line);
            if (commands != NULL){
                free_cmd(commands);
            }

            continue;
        }

        //built in commands
        if (strcmp(commands->argv[0], "cd") == 0) {
            cd(commands->argv);
        } else if (strcmp(commands->argv[0], "status") == 0) {
            //print status of last foreground process
            status_command(&shell);
        } else if (strcmp(commands->argv[0], "exit") == 0) {
            exit_command(&shell);
            free_cmd(commands);
            free(line);
            break;
        } else {
            if (fg_only_mode == 1) {
                commands->background = 0;
            }  

            //execute non built-in command 
            exec_command(&shell, commands);
        }
        free_cmd(commands);
        free(line);    
    }
    
    return 0;
}