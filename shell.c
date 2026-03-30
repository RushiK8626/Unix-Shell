/*   ________________________________________________
    |  MyShell - A simple Unix shell implementation  |
    |________________________________________________|
*/

# include <stdio.h>
# include <stdlib.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
# include <string.h>
# include <sys/types.h>
# include <sys/wait.h>
# include <limits.h>
# include <ctype.h>
# include <errno.h>
# include <signal.h>
# include <readline/readline.h>
# include <readline/history.h>

# define  MAX_TOKENS 100
# define  BUF_SIZE 1024
# define  INIT_TOK_SIZE 10
# define  MAX_VAR_NAME 64
# define  MAX_VAR_VALUE 256
# define  MAX_CMDS 100
# define  MAX_HISTORY 100
# define  MAX_LINE 1024
# define  STDIN 0
# define  STDOUT 1
# define  STDERR 2
# define  MAX_HISTORY 100
# define  MAX_CMD_LEN 1024
# define  HIST_FILE "myshell_hist.txt"

char* search_path = NULL;
struct var *var_list = NULL;
int use_cwd_prompt = 1;
char custom_prompt[128] = "";
volatile sig_atomic_t in_foreground = 1;

// Structure for redirection information
typedef struct {
    char *in_file;
    char *out_file;
    char *append_file;
    char *err_out_file;
    char *err_append_file;
    int in_redir;
    int out_redir;
    int append_redir;
    int err_out_redir;
    int err_append_redir;
} redirection_t;


// List node for variables 
struct var {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
    struct var *next;
};

typedef enum {
    RUNNING,
    STOPPED,
    COMPLETED,
    TERMINATED
} job_state;

// List node for jobs
typedef struct job{
	int job_id;
	pid_t pgid;
    pid_t pids[MAX_CMDS];
    int pid_count;
    int completed_count;
	char cmdline[1024];
	job_state state;
	int background;
    struct job* next;
}job;

// Global variables for pipes, commands, pids, redirections, and job list
int pipes[MAX_CMDS - 1][2];
char *cmds[MAX_CMDS];
pid_t pids[MAX_CMDS];
redirection_t redir[MAX_CMDS];
job *job_list = NULL;
int next_job_id = 1;

// Function declarations
char **tokenize(const char *line, int *tok_count);
void free_tokens(char **tokens);
ssize_t write_all(int fd, const char *buf, size_t count);
char *display_prompt();

void init_redirection(redirection_t *redir);
int parse_redirection(char **tokens, int *tok_count, redirection_t *redir);
void free_redirection(redirection_t *redir);

char* parse_assignments(char *line);
struct var* find_var(const char *name);
void set_var(const char *name, const char *value, int exported);
void unset_var(const char *name);
void expand_vars(char *str);


void create_process_pipeline(char *cmd[], int ncmds, int is_background, char *cmdline);
int handle_command_execution(char **tokens, int tok_count, int n, int ncmds);
void execute_command(char **args, redirection_t *redir);
void execute_path(char **args);
int process_type(char *line);

job* add_job(pid_t pids[], int pid_count, const char* cmdline, int is_background);
void remove_completed_jobs();
int remove_job_by_id(int job_id);
void update_job_state(pid_t pid, job_state new_state);
job* find_job_by_id(int job_id);
job* find_job_by_pgid(pid_t pgid);
job* find_job_by_pid(pid_t pid);
int get_job_count();
void print_jobs();

int execute_builtin(char *cmdline);
int is_builtin_command(const char *cmd);
void directory_change(char **tokens, int* tok_count);
void handle_exit_cmd(char *line);

void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigstp_handler(int sig);


/*   _____________________________________________
    | Variable management functions and utilities |
    |_____________________________________________|
*/

// Find a variable in the variable list by name. 
struct var* find_var(const char *name) {
    struct var *curr = var_list;
    while (curr) {
        if (strcmp(curr->name, name) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

// Set or update a variable in the variable list. 
void set_var(const char *name, const char *value, int exported) {
    struct var *v = find_var(name);
    if (v) {
        strncpy(v->value, value, MAX_VAR_VALUE);
        v->value[MAX_VAR_VALUE - 1] = '\0';
    } else {
        struct var *newv = malloc(sizeof(struct var));
        if(newv == NULL) {
            write_all(STDERR, "MyShell: Error: malloc failed\n", 30);
            return;
        }
        strncpy(newv->name, name, MAX_VAR_NAME);
        newv->name[MAX_VAR_NAME - 1] = '\0';
        strncpy(newv->value, value, MAX_VAR_VALUE);
        newv->value[MAX_VAR_VALUE - 1] = '\0';
        newv->next = var_list;
        var_list = newv;
    }
    if (exported) setenv(name, value, 1);
}

// Unset a variable from the variable list by name. 
void unset_var(const char *name) {
    struct var **curr = &var_list;
    while (*curr) {
        if (strcmp((*curr)->name, name) == 0) {
            struct var *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            unsetenv(name);
            return;
        }
        curr = &((*curr)->next);
    }
}

// Parse variable assignments at the start of a command line and return pointer to first non-assignment token
char* parse_assignments(char *line) {
    char *ptr = line;

    while (*ptr) {
        while (isspace(*ptr)) ptr++;
        if (*ptr == '\0') break;

        // Find the end of the current token (space or end of string)
        char *token_start = ptr;
        while (*ptr && !isspace(*ptr)) ptr++;
        int tok_len = ptr - token_start;

        // Check if this token contains '='
        char *eq = memchr(token_start, '=', tok_len);
        if (!eq) {
            // First non-assignment token → command starts here
            return token_start;
        }

        // Extract variable name and value
        int name_len = eq - token_start;
        int value_len = tok_len - name_len - 1;

        char *name = malloc(name_len + 1);
        char *value = malloc(value_len + 1);
        
        if (!name || !value) {
            free(name);
            free(value);
            write_all(STDERR, "MyShell: Error: malloc failed\n", 30);
            return token_start;
        }

        strncpy(name, token_start, name_len);
        name[name_len] = '\0';
        
        strncpy(value, eq + 1, value_len);
		value[value_len] = '\0';

	    // Store variable (not exported by default)
	    set_var(name, value, 0);
        
        if(strcmp(name, "PS1") == 0) {
        	if (strcmp(value, "\"\\w$\"") == 0) {
		        use_cwd_prompt = 1;
		    } else {
		        use_cwd_prompt = 0;
		        if (value[0] == '"' && value[strlen(value) - 1] == '"') {
		            value[strlen(value) - 1] = '\0';
		            strncpy(custom_prompt, value + 1, sizeof(custom_prompt));
		            custom_prompt[sizeof(custom_prompt) - 1] = '\0';
		        } else {
		            strncpy(custom_prompt, value, sizeof(custom_prompt));
		            custom_prompt[sizeof(custom_prompt) - 1] = '\0';
		        }
		    }
        }
        
        free(name);
        free(value);

        // Skip whitespace after this token
        while (isspace(*ptr)) ptr++;
    }

    return ptr;
}

// Expand variables in the input string (e.g. $VAR) using the variable list
void expand_vars(char *str) {
    size_t original_len = strlen(str);
    size_t result_size = original_len * 4 + 1024;
    char *result = malloc(result_size);
    if(result == NULL) {
        write_all(STDERR, "MyShell: Error: malloc failed\n", 30);
        return;
    }
    result[0] = '\0';

    char *p = str;
    size_t result_len = 0; 
    
    while (*p && result_len < result_size - 1) {
        if (*p == '$') {
            p++;
            char varname[MAX_VAR_NAME] = "";
            int i = 0;
            while ((isalnum(*p) || *p == '_') && i < MAX_VAR_NAME - 1) {
                varname[i++] = *p++;
            }
            varname[i] = '\0';
            
            struct var *v = find_var(varname);
            if (v) {
                size_t var_len = strlen(v->value);
                if (result_len + var_len < result_size - 1) {
                    strcat(result, v->value);
                    result_len += var_len;
                } else {
                    break;
                }
            }
        } else {
            if (result_len < result_size - 1) {
                result[result_len] = *p;
                result[result_len + 1] = '\0';
                result_len++;
            }
            p++;
        }
    }

    size_t copy_len = strlen(result);
    if (copy_len < original_len * 2 + 256) { 
        strcpy(str, result);
    } else {
        strncpy(str, result, original_len * 2 + 255);
        str[original_len * 2 + 255] = '\0';
    }
    
    free(result);
}

/*   ____________________________________
    | Tokenization and parsing functions |
    |____________________________________|
*/
// Tokenize the input line into an array of strings (tokens) based on whitespace and special characters. 
// Handles operators like >, >>, <, 2>, 2>> as separate tokens. 
// Returns a NULL-terminated array of tokens and sets the token count in the provided pointer.
char **tokenize(const char *line, int* tok_count) {
    
    size_t capacity = (size_t)INIT_TOK_SIZE;
    int count = 0;
    char **tokens = malloc(sizeof(char*) * capacity);
    if(!tokens) return NULL;
    
    const char *ptr = line;
    while(*ptr != '\0') {
        
        // Skip leading whitespaces
        while(*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r') ptr++;
        if(*ptr == '\0') break;
        
        const char* start = ptr;
        
        // Handle 2>>
        if(*ptr == '2' && *(ptr + 1) == '>' && *(ptr + 2) == '>') ptr += 3;
        
        // Handle >> and 2> as single
        else if((*ptr == '>' && *(ptr + 1) == '>') || (*ptr == '2' && *(ptr + 1) == '>')) ptr += 2;
        
        // Handle single character operators
        else if(*ptr == '>' || *ptr == '<') ptr++;
        
        // Regular tokens
        else while(*ptr != '\0' && *ptr != ' ' && *ptr != '\n' && *ptr != '\t' && *ptr != '\r' && *ptr != '<' && *ptr != '>') ptr++;
        size_t len = (size_t)(ptr - start);
        
        char *tok = malloc(len + 1);
        if(tok == NULL) {
            free_tokens(tokens);
            return NULL;
        }
        
        memcpy(tok, start, len);
        tok[len] = '\0';
        
        // If count of tokens is more than initial size of token array use realloc
        if((size_t)count + 1 >= capacity) {
            capacity *= 2;
            char **tmp = realloc(tokens, sizeof(char*) * capacity);
            if(tmp == NULL) {
                // Free tokens
                free_tokens(tokens);
                free(tok);
                return NULL;
            }
            tokens = tmp;
        }
        
        tokens[count++] = tok;
    }
    
    tokens[count] = NULL;
    if(tok_count) *tok_count = count;
    return tokens;
}


/*   ___________________________________
    | Redirection parsing and execution |
    |___________________________________|
 */

// Initialize redirection structure with default values (no redirection)    
void init_redirection(redirection_t *redir) {
    redir->in_file = NULL;
    redir->out_file= NULL;
    redir->append_file = NULL;
    redir->err_out_file = NULL;
    redir->err_append_file = NULL;
    redir->in_redir = 0;
    redir->out_redir = 0;
    redir->append_redir = 0;
    redir->err_out_redir = 0;
    redir->err_append_redir = 0;
}

// Free any dynamically allocated memory in the redirection structure
void free_redirection(redirection_t *redir) {
    if(redir->in_redir) free(redir->in_file);
    if(redir->out_redir) free(redir->out_file);
    if(redir->append_redir) free(redir->append_file);
    if(redir->err_out_redir) free(redir->err_out_file);
    if(redir->err_append_redir) free(redir->err_append_file);
}

// Parse redirection operators in the token list, populate the redirection structure, and remove redirection tokens from the list
int parse_redirection(char **tokens, int *tok_count, redirection_t *redir) {
    int new_count = 0;
    
    for(int i = 0; i < *tok_count; i++) {
        if(strcmp(tokens[i], "<") == 0) {
            if(i + 1 < *tok_count) {
                free(redir->in_file);
                redir->in_file = strdup(tokens[i + 1]);
                redir->in_redir = 1;
                free(tokens[i]);
                free(tokens[i + 1]);
                i++;
            }
            else {
                char *err = "MyShell: Syntax error: No input file\n";
                write_all(STDERR, err, strlen(err));
                return -1;
            }
        }
        else if(strcmp(tokens[i], ">") == 0) {
            if(i + 1 < *tok_count) {
                free(redir->out_file);
                redir->out_file = strdup(tokens[i + 1]);
                redir->out_redir = 1;
                free(tokens[i]);
                free(tokens[i + 1]);
                i++;
            }
            else {
                char *err = "MyShell: Syntax error: no output file\n";
                write_all(STDERR, err, strlen(err));
                return -1;
            }
        }
        else if(strcmp(tokens[i], ">>") == 0) {
            if(i + 1 < *tok_count) {
                free(redir->append_file);
                redir->append_file = strdup(tokens[i + 1]);
                redir->append_redir = 1;
                free(tokens[i]);
                free(tokens[i + 1]);
                i++;
            }
            else{
                char *err = "MyShell: Syntax error: no output file\n";
                write_all(STDERR, err, strlen(err));
                return -1;
            }
        }
        
        else if(strcmp(tokens[i], "2>") == 0) {
            if(i + 1 < *tok_count) {
                free(redir->err_out_file);
                redir->err_out_file = strdup(tokens[i + 1]);
                redir->err_out_redir = 1;
                free(tokens[i]);
                free(tokens[i + 1]);
                i++;
            }
            else {
                char *err = "MyShell: Syntax error: no error output file\n";
                write_all(STDERR, err, strlen(err));
                return -1;
            }
        }
        
        else if(strcmp(tokens[i], "2>>") == 0) {
            if(i + 1 < *tok_count) {
                free(redir->err_append_file);
                redir->err_append_file = strdup(tokens[i + 1]);
                redir->err_append_redir = 1;
                free(tokens[i]);
                free(tokens[i + 1]);
                i++;
            }
            else {
                char *err = "MyShell: Syntax error: no error output file\n";
                write_all(STDERR, err, strlen(err));
                return -1;
            }
        }
        
        else {
            tokens[new_count++] = tokens[i];
        }
    }
    tokens[new_count] = NULL;
    *tok_count = new_count;
    return 0;
}


/*   ___________________________________________________
    | Command Execution Functions and Pipeline Creation |
    |___________________________________________________|
*/

// Fork a child process, set up redirections, and execute the command in the child. Parent waits for child to finish.
void execute_command(char **args, redirection_t *redir) {
    pid_t pid = fork();
    if(pid < 0) {
        // Fork failed
        const char *err = "MyShell: fork failed\n";
        write_all(STDERR, err, strlen(err));
        return;
    }
    else if(pid == 0) {
        // Child process
        if(redir->in_redir) {
            int in_fd = open(redir->in_file, O_RDONLY);
            if(in_fd < 0) {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "MyShell: cannot open %s: %s\n", redir->in_file, strerror(errno));
                write_all(STDERR, msg, (size_t)n);
                exit(1);
            }
            dup2(in_fd, STDIN);
            close(in_fd);
            
        }
        
        if(redir->out_redir) {
            int out_fd = open(redir->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(out_fd < 0) {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "MyShell: cannot create %s: %s\n", redir->out_file, strerror(errno));
                write_all(STDERR, msg, (size_t)n);
                exit(1);
            }
            dup2(out_fd, STDOUT);
            close(out_fd);
        }
        
        if(redir->append_redir) {
            int append_fd = open(redir->append_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if(append_fd < 0) {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "MyShell: cannot append %s: %s\n", redir->append_file, strerror(errno));
                write_all(STDERR, msg, (size_t)n);
                exit(1);
            }
            dup2(append_fd, STDOUT);
            close(append_fd);
        }
        
        if(redir->err_out_redir) {
            int err_out_fd = open(redir->err_out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(err_out_fd < 0) {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "MyShell: cannot create %s: %s\n", redir->err_out_file, strerror(errno));
                write_all(STDERR, msg, (size_t)n);
                exit(1);
            }
            dup2(err_out_fd, STDERR);
            close(err_out_fd);
        }
        
        if(redir->err_append_redir) {
            int err_append_fd = open(redir->err_append_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if(err_append_fd < 0) {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "MyShell: cannot append %s: %s\n", redir->err_append_file, strerror(errno));
                write_all(STDERR, msg, (size_t)n);
                exit(1);
            }
            dup2(err_append_fd, STDERR);
            close(err_append_fd);
        }
        
        if(args[0] == NULL) exit(1);
        
        execute_path(args);
        
        char msg[256];
        int n = snprintf(msg, sizeof(msg), "MyShell: failed to execute %s: %s\n", args[0], strerror(errno));
        write_all(STDERR, msg, (size_t)n);

        exit(1);
    }
    else {
        // Parent process
        int status;
        while(1) {
            pid_t id = waitpid(pid, &status, 0);
            if (id < 0) {
                if (errno == EINTR) continue;
                break;
            }
            break;
        }
    }
}

// Search for the command in PATH and execute it. If the command contains a '/', treat it as a path and try to execute directly.
void execute_path(char **args) {
    if(args == NULL || args[0] == NULL) return;
    
    if(strchr(args[0],'/')) {
        execv(args[0], args);
        return;
    }
    
    struct var *path_var = find_var("PATH");
    char *env_path = path_var ? path_var->value : "/usr/bin";
    char *paths = strdup(env_path);
    if(paths == NULL) return;
    
    char *dir = strtok(paths, ":");
    
    while(dir) {
        char complete_path[PATH_MAX];
        int n = snprintf(complete_path, sizeof(complete_path), "%s/%s", dir, args[0]);
        
        if(n > 0 && n < PATH_MAX && access(complete_path, X_OK) == 0) {
            execv(complete_path, args);
            char msg[256];
            n = snprintf(msg, sizeof(msg), "MyShell: %s: Permission denied\n", args[0]);
            if(n > 0) write_all(STDERR, msg, (size_t)n);
            free(paths);
            _exit(1);
        }
        dir = strtok(NULL, ":");
    }
    
    free(paths);
    char msg[256];
    int n = snprintf(msg, sizeof(msg), "MyShell: %s: Command not found\n", args[0]);
    if(n > 0) write_all(STDERR, msg, (size_t)n);
    _exit(1);
}   

// Fork a child process, set up redirections, and execute the command in the child. Parent waits for child to finish.
int handle_command_execution(char **tokens, int tok_count, int n, int ncmds) {
	if(tok_count > 0) {        
       
        // Handle redirections
        init_redirection(&redir[n]);
        if(parse_redirection(tokens, &tok_count, &redir[n]) != 0) {
			free_redirection(&redir[n]);
			_exit(1);
		}
        
        // External commands
        if(tok_count > 0) execute_command(tokens, &redir[n]);
        
        free_redirection(&redir[n]);
        _exit(1);
      
    }
    _exit(1);
}

// Create a pipeline of processes for the given commands, set up pipes and redirections, and manage job control for foreground/background execution
void create_process_pipeline(char *cmd[], int ncmds, int is_background, char *cmdline) {
    int job_pid_count = 0;
    pid_t pgid = 0;  
    
    for(int n = 0; n < ncmds; n++) {
        char *cmdstart = parse_assignments(cmd[n]);
        
        while(isspace(*cmdstart)) cmdstart++;
        if(*cmdstart == '\0') {
            pids[n] = -1;
            continue;
        }
        
        if(is_builtin_command(cmdstart)) {
            if(ncmds == 1 && !is_background) {
                pids[n] = -1;
                execute_builtin(cmdstart);
                return;
            }
            continue;
        }
        
        pids[n] = fork();
        if(pids[n] < 0) {
            const char *err = "MyShell: fork failed\n";
            write_all(STDERR, err, strlen(err));
            return;
        }
        else if(pids[n] == 0) {
            // Child process
            
            // Set process group FIRST, before anything else
            if(n == 0) {
                setpgid(0, 0);
                pgid = getpid();
            } else {
                setpgid(0, pgid);
            }
            
            // Restore default signal handlers
            signal(SIGTSTP, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            
            // For background processes, redirect stdin from /dev/null
            if(is_background) {
                int null_fd = open("/dev/null", O_RDONLY);
                if(null_fd >= 0) {
                    dup2(null_fd, STDIN);
                    close(null_fd);
                }
            }
        
            // Pipe setup
            if(n > 0) {
                dup2(pipes[n - 1][0], STDIN);
            }
            
            if(n < ncmds - 1) {
                dup2(pipes[n][1], STDOUT);
            }
            
            // Close all pipes in child
            for(int j = 0; j < ncmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            // Tokenize and execute
            int tok_count = 0;
            char **tokens = tokenize(cmdstart, &tok_count);
            
            if(tokens == NULL) {
                const char *err = "MyShell: Tokenization failed\n";
                write_all(STDERR, err, strlen(err));
                _exit(1);
            }
            
            handle_command_execution(tokens, tok_count, n, ncmds);
            
            free_tokens(tokens);
            _exit(1);
        }
        else {
            // Parent process
            if(n == 0) {
                pgid = pids[n];
                setpgid(pids[n], pids[n]);
            } else {
                setpgid(pids[n], pgid);
            }
            job_pid_count++;
        }
    }
    
    if(job_pid_count > 0) {
        job* new_job = add_job(pids, job_pid_count, cmdline, is_background);
        
        // Give terminal control to foreground jobs
        if(!is_background && new_job) {
            tcsetpgrp(STDIN_FILENO, new_job->pgid);
        }
        if(new_job && is_background) {
            char msg[128];
            int n = snprintf(msg, sizeof(msg), "[%d] %s &\n", new_job->job_id, new_job->cmdline);
            write_all(STDOUT, msg, n);
        }
    }
}

// background process: 0
// foreground process: 1
int process_type(char *line) {
	int len = strlen(line);
	while(len > 0 && line[len - 1] == ' ' || line[len - 1] == '\t') len--;
	if(line[len - 1] == '&') {
		line[len - 1] = '\0';
		len--;
		while(len > 0 && line[len - 1] == ' ' || line[len - 1] == '\t') {
			line[len - 1] = '\0';
			len--;
		}
		return 0;
	}
	return 1;
}

/*   ________________________________________
    | Job management functions and utilities |
    |________________________________________|
*/

// Add a new job to the job list with the given process IDs, command line, and background/foreground status. Returns pointer to the new job or NULL on error.
job* add_job(pid_t pids[], int pid_count, const char* cmdline, int is_background) {
    if(pid_count > MAX_CMDS || pid_count < 0) {
        char* msg = "MyShell: Invalid number of processes\n";
        write_all(STDERR, msg, strlen(msg));
        return NULL;
    }
    
    job* new_job = malloc(sizeof(job));
    if(!new_job) {
        char *msg = "MyShell:Failed to allocated memory for new job\n";
        write_all(STDERR, msg, strlen(msg));
    }
    new_job->job_id = next_job_id++;
    new_job->pgid = pids[0];
    for(int i = 0; i < pid_count; i++) {
        new_job->pids[i] = pids[i];
    }
    new_job->pid_count = pid_count;
    new_job->completed_count = 0;
    strncpy(new_job->cmdline, cmdline, sizeof(new_job->cmdline) - 1);
    new_job->cmdline[sizeof(new_job->cmdline) - 1] = '\0';
    new_job->state = RUNNING;
    new_job->background = is_background;
    new_job->next = job_list;
    job_list = new_job;
    return new_job;
}

// Find a job in the job list by a given process ID. Returns pointer to the job or NULL if not found.
job* find_job_by_pid(pid_t pid) {
    job *current = job_list;
    while (current) {
        for (int i = 0; i < current->pid_count; i++) {
            if (current->pids[i] == pid) {
                return current;
            }
        }
        current = current->next;
    }
    return NULL;
}

// Find a job in the job list by a given process group ID. Returns pointer to the job or NULL if not found.
job* find_job_by_pgid(pid_t pgid) {
    job *current = job_list;
    while (current) {
        if (current->pgid == pgid) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Find a job in the job list by a given job ID. Returns pointer to the job or NULL if not found.
job* find_job_by_id(int job_id) {
    job *current = job_list;
    while (current) {
        if (current->job_id == job_id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Update the state of a job based on a process ID and new state. If the new state is COMPLETED, also update the completed count and potentially mark the entire job as completed.
void update_job_state(pid_t pid, job_state new_state) {
    job *j = find_job_by_pid(pid);
    if (!j) return;
    
    if (new_state == COMPLETED) {
        j->completed_count++;
        
        // If all processes in the job have completed, mark job as completed
        if (j->completed_count >= j->pid_count) {
            j->state = COMPLETED;
        }
        // Otherwise, job is still running (other processes active)
    } else {
        j->state = new_state;
    }
}

// Remove all jobs from the job list that are marked as COMPLETED. 
void remove_completed_jobs() {
    job **current = &job_list;
    
    while (*current) {
        if ((*current)->state == COMPLETED) {
            job *to_delete = *current;
            *current = (*current)->next;
            free(to_delete);
        } else {
            current = &((*current)->next);
        }
    }
}

// Remove a job from the job list by its job ID. Frees memory for the removed job and updates the linked list accordingly.
int remove_job_by_id(int job_id) {
    job **current = &job_list;
    
    while (*current) {
        if ((*current)->job_id == job_id) {
            job *to_delete = *current;
            *current = (*current)->next;
            free(to_delete);
            return 1;
        }
        current = &((*current)->next);
    }
    return 0;
}

// Get the count of active jobs (those that are not marked as COMPLETED) in the job list. Returns the number of active jobs.
int get_job_count() {
    int count = 0;
    job *current = job_list;
    while (current) {
        if (current->state != COMPLETED) {
            count++;
        }
        current = current->next;
    }
    return count;
}

// Print the list of active jobs to standard output. 
void print_jobs() {
    job *current = job_list;
    int has_jobs = 0;
    
    // Check if there are any active jobs
    job *temp = job_list;
    while (temp) {
        if (temp->state != COMPLETED) {
            has_jobs = 1;
            break;
        }
        temp = temp->next;
    }
    
    if (!has_jobs) {
    	char *msg = "There are no active jobs\n";
        write_all(STDOUT, msg, strlen(msg));
        return;
    }
    
    // Iterate through the list and print each job
    while (current) {
        if (current->state == COMPLETED) {
            current = current->next;
            continue;
        }
        
        char buffer[512];
        int pos = 0;
        
        // Print basic job info: [job_id] status pid state command
        pos += snprintf(buffer + pos, sizeof(buffer) - pos,
                       "[%d] %s %d %s %s\n",
                       current->job_id,
                       current->background ? "+" : "-",
                       current->pgid,
                       (current->state == RUNNING) ? "Running" :
                       (current->state == STOPPED) ? "Stopped" : "Terminated",
                       current->cmdline);
        
        write_all(STDOUT, buffer, pos);
        current = current->next;
    }
}

// Free all jobs in the job list and clear the list. 
void free_all_jobs() {
    while (job_list) {
        job *temp = job_list;
        job_list = job_list->next;
        free(temp);
    }
}


/*   _____________________________________
    | Built-in command handling functions |
    |_____________________________________|
*/

// Check if a command is a built-in command. Returns 1 if it is, 0 otherwise.
// This is used to determine whether to execute the command in the parent process (for built-ins) or fork a child process (for external commands).
int is_builtin_command(const char *cmd) {
    // Trim leading whitespace
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    
    if (strncmp(cmd, "cd", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) return 1;
    if (strncmp(cmd, "jobs", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return 1;
    if (strncmp(cmd, "exit", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return 1;
    if (strncmp(cmd, "fg", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) return 1;
    if (strncmp(cmd, "bg", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) return 1;
    if (strncmp(cmd, "PS1=", 4) == 0) return 1;
    if (strncmp(cmd, "history", 7) == 0 && (cmd[7] == ' ' || cmd[7] == '\0')) return 1;
    return 0;
}

// Execute built-in commands in the parent process
int execute_builtin(char *cmdline) {
    int tok_count = 0;
    char **tokens = tokenize(cmdline, &tok_count);
    
    if (tokens == NULL || tok_count == 0) {
        free_tokens(tokens);
        return 0;
    }
    
    // Handle cd
    if (strcmp(tokens[0], "cd") == 0) {
        directory_change(tokens, &tok_count);
        free_tokens(tokens);
        return 1;
    }
    
    // Handle jobs
    if (strcmp(tokens[0], "jobs") == 0) {
        remove_completed_jobs();  
        print_jobs();
        free_tokens(tokens);
        return 1;
    }
    
    // fg
	if (strcmp(tokens[0], "fg") == 0) {
		int job_id = 1;
		if(tok_count >= 2) {
		    char *id = tokens[1];
		    if(*id == '%') id++;
		    job_id = atoi(id);
		}
		
		job *j = find_job_by_id(job_id);
		if (!j) {
		    char msg[128];
		    int n = snprintf(msg, sizeof(msg), "MyShell: fg: %d: no such job\n", job_id);
		    write_all(STDERR, msg, n);
		} 
		else {
		    j->background = 0;
		    j->state = RUNNING;
		    
		    // Print the command being brought to foreground
		    char msg[256];
		    int n = snprintf(msg, sizeof(msg), "%s\n", j->cmdline);
		    write_all(STDOUT, msg, n);
		    
		    // Send SIGCONT to the entire process group
		    killpg(j->pgid, SIGCONT);
		    
		    // Give terminal control to the process group
		    tcsetpgrp(STDIN_FILENO, j->pgid);
		    
		    // Wait for all processes in the job
		    int status;
		    for(int i = 0; i < j->pid_count; i++) {
		        while(1) {
		            pid_t id = waitpid(j->pids[i], &status, WUNTRACED);
		            if (id < 0) {
		                if (errno == EINTR) continue;
		                break;
		            }
		            if (id > 0) {
		                if(WIFEXITED(status) || WIFSIGNALED(status)) {
		                    update_job_state(id, COMPLETED);
		                }
		                else if(WIFSTOPPED(status)) {
		                    update_job_state(id, STOPPED);
		                    j->background = 1;
		                    break;
		                }
		            }
		            break;
		        }
		    }
		    
		    // Return terminal control to shell
		    tcsetpgrp(STDIN_FILENO, getpgrp());
		}
		free_tokens(tokens);
		return 1;
	}
	
	// bg
	if (strcmp(tokens[0], "bg") == 0) {
		int job_id = 1;
		if(tok_count >= 2) {
		    char *id = tokens[1];
		    if(*id == '%') id++;
		    job_id = atoi(id);
		}
		
		job *j = find_job_by_id(job_id);
		if (!j) {
		    char msg[128];
		    int n = snprintf(msg, sizeof(msg), "MyShell: bg: %d: no such job\n", job_id);
		    write_all(STDERR, msg, n);
		} 
		else {
		    j->background = 1;
		    j->state = RUNNING;
		    
		    // Send SIGCONT to the entire process group
		    if(killpg(j->pgid, SIGCONT) < 0) {
		        char msg[128];
		        int n = snprintf(msg, sizeof(msg), "MyShell: bg: failed to continue job %d\n", job_id);
		        write_all(STDERR, msg, n);
		    } else {
		        char msg[256];
		        int n = snprintf(msg, sizeof(msg), "[%d]+ %s &\n", j->job_id, j->cmdline);
		        write_all(STDOUT, msg, n);
		    }
		}
		free_tokens(tokens);
		return 1;
	}
	
	// History Command
	 if (strcmp(tokens[0], "history") == 0) {
	 	int num = MAX_HISTORY;
	 	if(tok_count > 1) {
	 		int n = atoi(tokens[1]);
	 		if(n) num = n;
	 	}
	 	
        HIST_ENTRY **hist_list = history_list();
        if (hist_list) {
      		int count = 0;
            for (int i = 0; hist_list[i]; i++) {
	        	if(count >= num) break;
                printf("%d  %s\n", i + history_base, hist_list[i]->line);
                count++;
            }
        }
        return 1;
    }
    
    // Handle PS1
    if (strncmp(tokens[0], "PS1=", 4) == 0) {
        char *val = tokens[0] + 4;
        if (strcmp(val, "\"\\w$\"") == 0) {
            use_cwd_prompt = 1;
        } else {
            use_cwd_prompt = 0;
            if (val[0] == '"' && val[strlen(val) - 1] == '"') {
                val[strlen(val) - 1] = '\0';
                strncpy(custom_prompt, val + 1, sizeof(custom_prompt));
                custom_prompt[sizeof(custom_prompt) - 1] = '\0';
            } else {
                strncpy(custom_prompt, val, sizeof(custom_prompt));
                custom_prompt[sizeof(custom_prompt) - 1] = '\0';
            }
        }
        free_tokens(tokens);
        return 1;
    }
    
    free_tokens(tokens);
    return 0;
}

// cd command handler
void directory_change(char **tokens, int* tok_count) {
    if (*tok_count >= 2) {
        if (chdir(tokens[1]) != 0) {
            char msg[256];
            int n = snprintf(msg, sizeof(msg), "MyShell: cd failed: %s\n", strerror(errno));
            if (n > 0) write_all(STDERR, msg, (size_t)n);
        }
    }
    else {
        char *home = getenv("HOME");
        if(home){
            if(chdir(home) != 0 ) {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "MyShell: cd failed: %s\n", strerror(errno));
                if (n > 0) write_all(STDERR, msg, (size_t)n);
            }
        }
    }
}

// exit command handler
void handle_exit_cmd(char *line) {
	char *trimmed = line;
    while(*trimmed == ' ' || *trimmed == '\t') trimmed++;
	if(strncmp(trimmed, "exit", 4) == 0) {
		trimmed += 4;
		while(*trimmed == ' ' || *trimmed == '\t') trimmed++;
		if(*trimmed == '\0') {
			free(line);
			if(search_path) free(search_path);
		}
		write_all(STDOUT, "\nGOOD BYE\n", 10);
		exit(0);
	}
}

// history command handler 
void show_history(int num) {
    int fd = open(HIST_FILE, O_RDONLY);
    if (fd == -1) {
       	char msg[256];
       	int n = snprintf(msg, sizeof(msg), "MyShell: Failed to open: %s\n", HIST_FILE);
       	if(n > 0) write_all(STDERR, msg, strlen(msg));
        return;
    }

    off_t filesize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    char *buffer = malloc(filesize + 1);
    if (!buffer) {
        close(fd);
        return;
    }

    read(fd, buffer, filesize);
    buffer[filesize] = '\0';
    close(fd);

    // Split into lines
    int count = 0;
    char *lines[MAX_HISTORY];
    char *line = strtok(buffer, "\n");
    while (line && count < MAX_HISTORY) {
        lines[count++] = line;
        line = strtok(NULL, "\n");
    }

    int start = 0;
    if (num > 0 && num < count)
        start = count - num;

    for (int i = start; i < count; i++)
        printf("%d  %s\n", i + 1, lines[i]);

    free(buffer);
}


/*   ________________________________________
    | Signal handlers for job control and UI |
    |________________________________________|
*/

// SIGCHLD signal handler to reap child processes and update job states accordingly. 
// Handles process completion, stopping, and continuation, and prints appropriate messages for background jobs.
void sigchld_handler(int sig) {
    int saved_errno = errno;
    pid_t pid;
    int status;
    
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        job *j = find_job_by_pid(pid);
        
        if(j) {
            if(WIFEXITED(status) || WIFSIGNALED(status)) {
                update_job_state(pid, COMPLETED);
                if(j->state == COMPLETED && j->background) {
                    char msg[256];
                    int n = snprintf(msg, sizeof(msg), 
                                   "\n[%d] Done %s\n", 
                                   j->job_id, j->cmdline);
                    if(n > 0 && n < 256) {
                        write(STDOUT_FILENO, msg, n);
                    }
                    char *prompt = display_prompt();
                    write_all(STDOUT, prompt, strlen(prompt));
                }
            }
            else if(WIFSTOPPED(status)) {
                j->state = STOPPED;
                j->background = 1;
                
                char msg[256];
                int n = snprintf(msg, sizeof(msg), 
                               "\n[%d]+ Stopped %s\n", 
                               j->job_id, j->cmdline);
                if(n > 0 && n < 256) {
                    write(STDOUT_FILENO, msg, n);
                }
            }
            else if(WIFCONTINUED(status)) {
                j->state = RUNNING;
            }
        }
    }
    
    errno = saved_errno;
}

// Control + C signal handler
void sigint_handler(int sig) {
	write_all(STDOUT, "\n", 1);
}

/*   ____________________________________
    | Other Helper and Utility Functions |
    |____________________________________|
*/
// Display the shell prompt. If use_cwd_prompt is set, display the current working directory in the prompt. 
// Otherwise, display a custom prompt defined by the user. Returns a pointer to the prompt string.
char *display_prompt() {
	static char prompt[1024];
    if(use_cwd_prompt) {
        char cwd[PATH_MAX];
        if(getcwd(cwd, sizeof(cwd)) == NULL) {
            char msg[256];
            int n = snprintf(msg, sizeof(msg), "myShell: getcwd failed: %s\n", strerror(errno));
            if (n > 0) write_all(STDERR, msg, (size_t)n);
            strcpy(cwd, "$ ");
        }
        int n = snprintf(prompt, sizeof(prompt), "\001\033[1;32m\002MyShell\001\033[0m:\033[1;34m\002%s>\001\033[0m\002 ", cwd);
        return prompt;
    }
    else {
    	return custom_prompt;
    }
}

// Helper function for write calls to ensure all data is written, handling partial writes and EINTR errors. Returns total bytes written or -1 on error.
ssize_t write_all(int fd, const char *buf, size_t count) {
    size_t written = 0;
    const char *p = buf;
    while(written < count) {
        ssize_t n = write(fd, p + written, count - written);
        if(n < 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }
    return (ssize_t)written;
}

// Free the tokens
void free_tokens(char **tokens) {
    if(tokens == NULL) return;
    for (int i = 0; tokens[i] != NULL; ++i) free(tokens[i]);
    free(tokens);
}


/*  _________________________________________
    | Main function and shell initialization |
    |________________________________________|
*/
int main() {
    // Put the shell in its own process group
    pid_t shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    
    // Take control of the terminal
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    
    // Set up signal handling
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, SIG_IGN);    // Shell ignores Ctrl+C
    signal(SIGTSTP, SIG_IGN);   // Shell ignores Ctrl+Z
    signal(SIGTTOU, SIG_IGN);   
	signal(SIGTTIN, SIG_IGN);
    
    char *env_path = getenv("PATH");
    if (env_path) {
        set_var("PATH", env_path, 1);
        search_path = strdup(env_path);
    } else {
        set_var("PATH", "usr/bin", 1);
        search_path = strdup("usr/bin");
    }
    
    using_history();
    read_history(HIST_FILE);
    
    // Run shell loop
    while(1) {
    	remove_completed_jobs();
    	
        // Display initial prompt every time 
        // Read the input
        char *line = readline(display_prompt());
        if(!line) {
        	write_all(STDOUT, "\n", 1);
 			break;
 		}
 		
 		if(line) {
 			add_history(line); 
 			append_history(1, HIST_FILE);
 		}    
 
        handle_exit_cmd(line);
        
        expand_vars(line);
        
        // If only assignments were found, continue to next iteration
        while(isspace(*line)) line++;
        if(*line == '\0') {
            free(line);
            continue;
        }
        
        char *line_cpy = strdup(line);
        int has_pipe = (strchr(line_cpy, '|') != NULL);
        free(line_cpy);
		
        int fg_process = process_type(line);
        
        char *original_cmd = strdup(line);
        int ncmds = 0;
        char *token = strtok(line, "|");
        while(token && ncmds < MAX_CMDS) {
        	cmds[ncmds++] = token;
        	token = strtok(NULL, "|");
        }
        
        // Set up pipes for the pipeline
        for(int i = 0; i < ncmds - 1; ++i) {
        	if(pipe(pipes[i]) == -1) {
				char *msg = "MyShell: Failed to create pipe\n";
				write_all(STDERR, msg, strlen(msg));
				free(line);
				free(token);
				free(original_cmd);
				continue;
			}
		}
		
        create_process_pipeline(cmds, ncmds, !fg_process, original_cmd);
      
        
        // close all pipes
        for(int i = 0; i < ncmds - 1; i++) {
			close(pipes[i][0]);
			close(pipes[i][1]);
		}
        
        // If it's a foreground process, wait for it to complete or stop. If it's a background process, just print the prompt and continue.
        if(fg_process) {
        	int status;
			for(int i = 0; i < ncmds; i++) {
				if(pids[i] < 0) continue;
				while(1) {
					pid_t id = waitpid(pids[i], &status, WUNTRACED);
					if (id < 0) {
					    if (errno == EINTR) continue;
					    break;
					}
					if (id > 0) {
						if(WIFEXITED(status) || WIFSIGNALED(status)) {
							update_job_state(id, COMPLETED);
						}
						else if(WIFSTOPPED(status)) {
							update_job_state(id, STOPPED); 
					        job *j = find_job_by_pid(id);
					        if(j) {
					            j->background = 1;
					        }
							break;
						}
					}
					break;
				}
			}
			// Return terminal control to shell after foreground job stops/completes
    		tcsetpgrp(STDIN_FILENO, getpgrp());
        }
        
        in_foreground = 1;
        if(!fg_process) usleep(10000);

        free(line);
    }
    
    // Clean up before exiting
    free_all_jobs();
    if(search_path) free(search_path);
    struct var *curr = var_list;
    while(curr) {
        struct var *next = curr->next;
        free(curr);
        curr = next;
    }
    write_history(HIST_FILE);
    return 0;
}
