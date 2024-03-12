// Done: simple commands, background touch, rm, cat < file, ls -l > filename, cd, 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <limits.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>

#define MAX_ARGUMENTS 64
#define MAX_JOBS 64
#define MAX_COMMAND_LENGTH 1024

// Global variables
pid_t jobs[MAX_JOBS];
int num_jobs = 0;

// Function declarations
void execute_command(char *command);
void execute_ls(char *arguments[]);
void handle_signals(int signo);
void parse_commands(char *input);
void execute_builtin(char *command, char *arguments[]);
void display_jobs();
void bring_to_foreground(pid_t job_id);
void send_signal_to_job(int signo, pid_t job_id);
void execute_with_redirection(char *command, int in_fd, int out_fd, int err_fd);
void execute_with_pipes(char *command1, char *command2);
void execute_with_input_redirection(char *command, char *filename);
void execute_with_output_append(char *command, char *filename);
void execute_with_output_error_append(char *command, char *filename);
void yy_scan_string(const char *str);

// Helper functions
void print_error(char *message);

// Parse command line arguments
void parse_command(char *command, char *arguments[])
{
    char *token = strtok(command, " \t\n");
    int i = 0;
    while (token != NULL && i < MAX_ARGUMENTS - 1)
    {
        arguments[i++] = token;
        token = strtok(NULL, " \t\n");
    }
    arguments[i] = NULL;
}

// Function to check if input redirection is present in the arguments
int has_input_redirection(char *arguments[])
{
    int i = 0;
    while (arguments[i] != NULL)
    {
        if (strcmp(arguments[i], "<") == 0 && arguments[i + 1] != NULL)
        {
            // Input redirection found, return its index
            return i;
        }
        i++;
    }
    // Input redirection not found
    return -1;
}

// Function to check if output redirection is present in the arguments
int has_output_redirection(char *arguments[])
{
    int i = 0;
    while (arguments[i] != NULL)
    {
        if (strcmp(arguments[i], ">") == 0 && arguments[i + 1] != NULL)
        {
            // Output redirection found, return its index
            return i;
        }
        i++;
    }
    // Output redirection not found
    return -1;
}


void execute_command(char *command)
{
    char *arguments[MAX_ARGUMENTS];
    parse_command(command, arguments);

    if (arguments[0] == NULL)
        return;

    int background = 0;
    int i = 0;
    while (arguments[i] != NULL)
    {
        if (strcmp(arguments[i], "&") == 0 && arguments[i + 1] == NULL)
        {
            background = 1;
            arguments[i] = NULL;
            break;
        }
        i++;
    }

    // Check if input redirection is present
    int redirection_index = has_input_redirection(arguments);
    if (redirection_index != -1)
    {
        char *filename = arguments[redirection_index + 1];
        execute_with_input_redirection(arguments[0], filename);
        printf("\n");
        return; // Exit the function to avoid forking a new process
    }

    // Check if output redirection is present
    int redirection_index_out = has_output_redirection(arguments);
    if (redirection_index_out != -1)
    {
        char *filename = arguments[redirection_index_out + 1];
        // Open the file for writing, create if not exist, truncate if exist
        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1)
        {
            perror("ish");
            return;
        }
        // Save stdout
        int saved_stdout = dup(STDOUT_FILENO);
        // Redirect stdout to the file
        dup2(fd, STDOUT_FILENO);
        close(fd);
        // Execute the command
        execute_command(arguments[0]);
        // Restore stdout
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    else
    {
        // Check if the command is a built-in command
        if (strcmp(arguments[0], "cd") == 0 ||
            strcmp(arguments[0], "exit") == 0 ||
            strcmp(arguments[0], "bg") == 0 ||
            strcmp(arguments[0], "fg") == 0 ||
            strcmp(arguments[0], "jobs") == 0 ||
            strcmp(arguments[0], "kill") == 0 ||
            strcmp(arguments[0], "setenv") == 0 ||
            strcmp(arguments[0], "unsetenv") == 0)
        {
            // If it's a built-in command, execute it directly
            execute_builtin(arguments[0], arguments);
        }
        else
        {
            // Search for the executable file in /usr/bin
            char executable_path[MAX_COMMAND_LENGTH];
            snprintf(executable_path, sizeof(executable_path), "/usr/bin/%s", arguments[0]);

            if (access(executable_path, X_OK) == 0)
            {
                // Found the executable file in /usr/bin, execute it
                pid_t pid = fork();
                if (pid == 0)
                {
                    // Child process
                    execv(executable_path, arguments);
                    perror("ish");
                    exit(EXIT_FAILURE);
                }
                else if (pid < 0)
                {
                    // Fork failed
                    perror("ish");
                }
                else
                {
                    // Parent process
                    if (!background)
                    {
                        waitpid(pid, NULL, 0);
                    }
                    else
                    {
                        // Print job number and process ID
                        printf("[%d] %d\n", num_jobs + 1, pid);
                        jobs[num_jobs++] = pid;
                    }
                }
            }
            else
            {
                // Executable file not found in /usr/bin
                printf("ish: %s: command not found\n", arguments[0]);
            }
        }
    }
}


// Built-in command: cd
void ish_cd(char *directory)
{
    if (directory == NULL || strcmp(directory, "~") == 0)
    {
        // Change to home directory
        struct passwd *pw = getpwuid(getuid());
        directory = pw->pw_dir;
    }
    if (chdir(directory) == -1)
    {
        perror("ish");
    }
}

// Built-in command: exit
void ish_exit()
{
    // Clean up jobs
    for (int i = 0; i < num_jobs; i++)
    {
        kill(jobs[i], SIGTERM);
    }
    exit(EXIT_SUCCESS);
}

// Built-in command: bg
void ish_bg()
{
    // Check if there are any background jobs
    if (num_jobs == 0)
    {
        printf("No background jobs\n");
        return;
    }

    // Find the last background job
    pid_t job_id = jobs[num_jobs - 1];

    // Send a continue signal to the job
    if (kill(job_id, SIGCONT) == -1)
    {
        perror("ish");
    }
}

// Built-in command: fg
void ish_fg()
{
    // Check if there are any background jobs
    if (num_jobs == 0)
    {
        printf("No background jobs\n");
        return;
    }

    // Find the last background job
    pid_t job_id = jobs[num_jobs - 1];

    // Bring the job to the foreground
    bring_to_foreground(job_id);
}

// Function to bring a job to the foreground
void bring_to_foreground(pid_t job_id)
{
    // Send a continue signal to the job
    if (kill(job_id, SIGCONT) == -1)
    {
        perror("ish");
        return;
    }

    // Wait for the job to finish
    waitpid(job_id, NULL, WUNTRACED);

    // Remove the job from the list
    for (int i = 0; i < num_jobs; i++)
    {
        if (jobs[i] == job_id)
        {
            for (int j = i; j < num_jobs - 1; j++)
            {
                jobs[j] = jobs[j + 1];
            }
            num_jobs--;
            break;
        }
    }
}

// Built-in command: jobs
void ish_jobs()
{
    printf("Jobs:\n");
    for (int i = 0; i < num_jobs; i++)
    {
        printf("[%d] %d\n", i + 1, jobs[i]);
    }
}

// Built-in command: kill
void ish_kill(char *job_id)
{
    if (job_id == NULL)
    {
        printf("Usage: kill <job_id>\n");
        return;
    }
    int id = atoi(job_id);
    if (id > 0 && id <= num_jobs)
    {
        kill(jobs[id - 1], SIGTERM);
    }
    else
    {
        printf("Invalid job ID\n");
    }
}

// Built-in command: setenv
void ish_setenv(char *var, char *value)
{
    if (var == NULL)
    {
        printf("Usage: setenv <variable> [value]\n");
        return;
    }
    if (value == NULL)
    {
        value = "";
    }
    setenv(var, value, 1);
}

// Built-in command: unsetenv
void ish_unsetenv(char *var)
{
    if (var == NULL)
    {
        printf("Usage: unsetenv <variable>\n");
        return;
    }
    unsetenv(var);
}

// Execute built-in commands
void execute_builtin(char *command, char *arguments[])
{
    if (strcmp(command, "cd") == 0)
    {
        ish_cd(arguments[1]);
    }
    else if (strcmp(command, "exit") == 0)
    {
        ish_exit();
    }
    else if (strcmp(command, "bg") == 0)
    {
        ish_bg();
    }
    else if (strcmp(command, "fg") == 0)
    {
        ish_fg();
    }
    else if (strcmp(command, "jobs") == 0)
    {
        ish_jobs();
    }
    else if (strcmp(command, "kill") == 0)
    {
        ish_kill(arguments[1]);
    }
    else if (strcmp(command, "setenv") == 0)
    {
        ish_setenv(arguments[1], arguments[2]);
    }
    else if (strcmp(command, "unsetenv") == 0)
    {
        ish_unsetenv(arguments[1]);
    }
    else
    {
        printf("Command not found\n");
    }
}


// Execute command with input redirection
void execute_with_input_redirection(char *command, char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd == -1)
    {
        perror("ish");
        return;
    }
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        dup2(fd, STDIN_FILENO); // Redirect stdin to file
        close(fd);
        execlp(command, command, NULL); // Execute command
        perror("ish");
        exit(EXIT_FAILURE);
    }
    else if (pid < 0)
    {
        // Fork failed
        perror("ish");
        return;
    }
    // Parent process
    waitpid(pid, NULL, 0);
    close(fd);
}

// Handle signals
void handle_signals(int signo)
{
    if (signo == SIGTERM)
    {
        // Handle SIGTERM signal
        // Clean up jobs
        for (int i = 0; i < num_jobs; i++)
        {
            kill(jobs[i], SIGTERM);
        }
        exit(EXIT_SUCCESS);
    }
    else if (signo == SIGINT)
    {
        // Ignore the SIGINT signal
        signal(SIGINT, SIG_IGN);
    }
    else if (signo == SIGTSTP)
    {
        // Ignore the SIGTSTP signal
        signal(SIGTSTP, SIG_IGN);
    }
}


void parse_commands(char *input)
{
    char *command;
    char *saveptr;

    // Tokenize input string by semicolon
    command = strtok_r(input, ";", &saveptr);
    while (command != NULL)
    {
        // Trim leading and trailing whitespace from command
        char *trimmed_command = command;
        while (*trimmed_command && (*trimmed_command == ' ' || *trimmed_command == '\t'))
            trimmed_command++;
        int len = strlen(trimmed_command);
        while (len > 0 && (trimmed_command[len - 1] == ' ' || trimmed_command[len - 1] == '\t'))
            trimmed_command[--len] = '\0';

        // Execute the trimmed command
        if (strlen(trimmed_command) > 0)
        {
            execute_command(trimmed_command);
        }

        // Move to the next command
        command = strtok_r(NULL, ";", &saveptr);
    }
}



// Execute commands with pipes
void execute_with_pipes(char *command1, char *command2)
{
    int pipefd[2];
    if (pipe(pipefd) == -1)
    {
        perror("ish");
        return;
    }

    pid_t pid1, pid2;
    pid1 = fork();
    if (pid1 == 0)
    {
        // Child process: execute command1 and write output to pipe
        close(pipefd[0]);               // Close read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe
        close(pipefd[1]);               // Close write end

        execute_command(command1);
        exit(EXIT_SUCCESS);
    }
    else if (pid1 < 0)
    {
        // Fork failed
        perror("ish");
        return;
    }

    // Parent process
    pid2 = fork();
    if (pid2 == 0)
    {
        // Child process: execute command2 and read input from pipe
        close(pipefd[1]);              // Close write end
        dup2(pipefd[0], STDIN_FILENO); // Redirect stdin to pipe
        close(pipefd[0]);              // Close read end

        execute_command(command2);
        exit(EXIT_SUCCESS);
    }
    else if (pid2 < 0)
    {
        // Fork failed
        perror("ish");
        return;
    }

    // Parent process: close pipe ends and wait for children
    close(pipefd[0]);
    close(pipefd[1]);
    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
}

// Display error message
void print_error(char *message)
{
    fprintf(stderr, "Error: %s\n", message);
}

// Function to read .ishrc file 
void read_ishrc() {
    FILE *file = fopen(".ishrc", "r");
    if (file != NULL) {
        char line[MAX_COMMAND_LENGTH];
        while (fgets(line, sizeof(line), file)) {
            parse_commands(line);
        }
        fclose(file);
    }
}


int main()
{
    read_ishrc();
    signal(SIGINT, handle_signals);
    signal(SIGTSTP, handle_signals);
    char input[MAX_COMMAND_LENGTH];

    while (1)
    {
        char cwd[MAX_COMMAND_LENGTH];
        if (getcwd(cwd, sizeof(cwd)) == NULL)
        {
            perror("ish");
            return EXIT_FAILURE;
        }

        char hostname[HOST_NAME_MAX + 1];
        if (gethostname(hostname, sizeof(hostname)) == -1)
        {
            perror("ish");
            return EXIT_FAILURE;
        }

        printf("%s:%s%% ", hostname, cwd);
        fflush(stdout);
        
        fgets(input, MAX_COMMAND_LENGTH, stdin);

        // Check if the command contains a pipe
        char *pipe_token = "|";
        char *command1 = strtok(input, pipe_token);
        char *command2 = strtok(NULL, pipe_token);

        if (command2 != NULL)
        {
            // If command2 is not NULL, execute commands with pipes
            execute_with_pipes(command1, command2);
        }
        else
        {
            // Otherwise, parse and execute single command
            parse_commands(input);
        }
    }

    return 0;
}
