/*
*	Program that shows sizes of given files on the disk.
*
*	Author: Leo Juneblad (c19lsd)
*
* Version: 2.0
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <semaphore.h>

struct dir_info {
	int parent_id;
	char *name;
};

struct thread_info {
	int thread_max;
	int thread_id;
};

void *thread_func(void *arg);
void run_threads(pthread_t *threads, int thread_amount);
int get_directory_size(struct dir_info file);
int get_available_file_size(struct dir_info file, struct dirent *dirent_t);
struct dir_info get_available_file(void);
void set_available_file(struct dir_info f);
void initialize(char **argv, int thread_max);
void initialize_files(struct dir_info dir);
struct stat get_stat(char *file);
void print(char **files);
bool is_dir(struct stat file_info);
void join_threads(pthread_t *threads, int thread_amount);
void free_memory(void);

//----mutexes and semaphores-----
pthread_mutex_t size_lock;
pthread_mutex_t status_lock;
pthread_mutex_t available_lock;
sem_t available_sem;

//-------global variables--------
int *total_sizes;
int nr_available_files = 0;
int exit_status = 0;
bool *done_threads;
bool done;
struct dir_info *available_files;

int main(int argc, char *argv[]) {
	char *p;
  long temp;
	int opt;

	int thread_amount = 1;

  if(argc < 2) {
    fprintf(stderr, "usage: ./mdu file [files]\n");
    exit(EXIT_FAILURE);
  }

	//Get number of threads from user input
	while ((opt = getopt(argc, argv, "j:")) != -1) {
		switch (opt) {
			case 'j':
			temp = strtol(optarg, &p, 10);
			thread_amount = temp;
			break;
		}
	}

  pthread_t threads[thread_amount];
  initialize(argv, thread_amount);

  if(nr_available_files > 0) {
		run_threads(threads, thread_amount);
  }

  print(argv);

  free_memory();

	//If there were errors in the threads the exit status is set to 1
	return exit_status;
}

/*
*	Start a number of pthreads equal to the number given by the user or 1.
*
*	@threads: The array of threads to be created.
*	@thread_amount: Number of threads to be created.
*
*	Returns: Nothing.
*
*/
void run_threads(pthread_t *threads, int thread_amount) {

	struct thread_info thread_arg[thread_amount];

	for(int i = 0; i < thread_amount; i++) {

		//Give each new thread a unique id and and pass in the total number of
		//threads
		thread_arg[i].thread_id = i;
		thread_arg[i].thread_max = thread_amount;

		if((pthread_create(&threads[i], NULL, thread_func, &thread_arg[i])) != 0) {
			perror("pthread_create: ");
			exit(EXIT_FAILURE);
		}
	}

	join_threads(threads, thread_amount);
}

/*
*	Function that runs in threads. Gets files and adds size values of the to the
* array of sizes.
*
*	@arg: The thread argument is a strct containgin the max number of threads and
* 			the id of the current thread.
*
*	Returns: arg.
*
*/
void *thread_func(void *arg) {

	struct thread_info info = *(struct thread_info*) arg;
	struct dir_info f;
	int size;

	//While loop can only be exited from inside once it is determined that all
	//threads have finished their work.
	while(1) {
		//Lock the use if 'nr_available_files' and 'done' global variables then
		//check if threads are done.
		pthread_mutex_lock(&available_lock);
		done_threads[info.thread_id] = true;
		if(nr_available_files == 0) {
			done = true;
			for(int i = 0; i < info.thread_max; i++) {
				if(done_threads[i] == false) {
					done = false;
					break;
				}
			}
			//If all threads are done, signal all other threads incase one is waiting
			//on the semaphore, and exit the loop.
			if(done) {
				for(int i = 0; i < info.thread_max; i++) {
					sem_post(&available_sem);
				}
				pthread_mutex_unlock(&available_lock);
				break;
			}
		}
		pthread_mutex_unlock(&available_lock);

		sem_wait(&available_sem);
		//If all threads are done exit.
		if(done) {
			break;
		}

		done_threads[info.thread_id] = false;

		//Get the size of a directory.
		f = get_available_file();
		size = get_directory_size(f);
		pthread_mutex_lock(&size_lock);
		total_sizes[f.parent_id] += size;
		pthread_mutex_unlock(&size_lock);
	}

	return arg;
}

/*
*	Finds and returns the size of a directory.
*
*	@file: A struct containing the name of a file and the id of that files parent.
*
*	Returns: The size of the given directory if it could be opened and 0
*	otherwise.
*
*/
int get_directory_size(struct dir_info file) {
  int size = 0;

  DIR *dir_t;
	struct dirent *dirent_t;
  struct stat file_stat;

	//If a file cannot be found, set the exit status and continue past the
	//problematic file.
  if(lstat(file.name, &file_stat) < 0) {
    fprintf(stderr, "unable to stat: '%s'", file.name);
		perror("");
		pthread_mutex_lock(&status_lock);
		exit_status = 1;
		pthread_mutex_unlock(&status_lock);
		free(file.name);
		return 0;
  }

  if((dir_t = opendir(file.name)) == NULL) {
		fprintf(stderr, "du: cannot read directory '%s': ", file.name);
		perror("");
		pthread_mutex_lock(&status_lock);
		exit_status = 1;
		pthread_mutex_unlock(&status_lock);
		free(file.name);
		return 0;
	}

	//Read all files in directory.
  while((dirent_t = readdir(dir_t)) != NULL) {
    size += get_available_file_size(file, dirent_t);
  }

	//The given directory has been measured and can be freed.
	free(file.name);

  if(closedir(dir_t) < 0) {
		fprintf(stderr, "closedir error: ");
    perror("");
		pthread_mutex_lock(&status_lock);
		exit_status = 1;
		pthread_mutex_unlock(&status_lock);
  }
  return size;
}

/*
*	Gets the size of a given file that is not a directory
*
*	@file: Struct containing the name of a file and the id of its parent.
*	@dirent_t: The dirent struct of the current directory.
*
*	Returns: The size of the given file.
*
*/
int get_available_file_size(struct dir_info file, struct dirent *dirent_t) {
  struct stat file_stat;
	char *temp;

  if((strcmp(dirent_t->d_name, ".") == 0 || strcmp(dirent_t->d_name, "..") == 0)) {
    return 0;
  }

  if((temp = malloc((strlen(file.name) + strlen(dirent_t->d_name) + 2) * sizeof(char))) == NULL) {
    perror("malloc: ");
    exit(EXIT_FAILURE);
  }

  sprintf(temp, "%s/%s", file.name, dirent_t->d_name);

	if(lstat(temp, &file_stat) < 0) {
		fprintf(stderr, "unable to stat: '%s': ", file.name);
		perror("");
		pthread_mutex_lock(&status_lock);
		exit_status = 1;
		pthread_mutex_unlock(&status_lock);
		free(temp);
		return 0;
	}

	//If the file is a directory add it to the array and signal that there is a
	//file available.
  if(is_dir(file_stat)) {
    struct dir_info temp_dir;
    temp_dir.name = temp;
    temp_dir.parent_id = file.parent_id;
    set_available_file(temp_dir);
		sem_post(&available_sem);
  }
  else {
    free(temp);
  }


  return file_stat.st_blocks;
}

//What to do with the first files given.

/*
*	Gets a file struct from the global array of file structs.
*
*	Returns: A file struct.
*
*/
struct dir_info get_available_file(void) {
	struct dir_info f;
	pthread_mutex_lock(&available_lock);
	nr_available_files--;
	f = available_files[nr_available_files];
	pthread_mutex_unlock(&available_lock);

	return(f);
}

/*
*	Adds a file struct to the global array of file structs.
*
*	@f: The file struct to be put into the array.
*
*	Returns: Nothing if succesfull.
*
*/
void set_available_file(struct dir_info f) {
	pthread_mutex_lock(&available_lock);
	nr_available_files++;
	if((available_files = realloc(available_files, nr_available_files * sizeof(struct dir_info))) == NULL) {
		perror("realloc 'available_files': ");
		exit(EXIT_FAILURE);
	}
	available_files[nr_available_files - 1] = f;
	pthread_mutex_unlock(&available_lock);
}

/*
*	initialize the mutexes and global variables.
*
*	@thread_max: The number of threads to run in the program.
*
*	Returns: Nothing if succesfull.
*
*/
void initialize(char **argv, int thread_max) {
  if((pthread_mutex_init(&status_lock, NULL) != 0) ||
     (pthread_mutex_init(&size_lock, NULL) != 0) ||
     (pthread_mutex_init(&available_lock, NULL) != 0)) {
  	perror("pthread_mutex_init: ");
  }

	if(sem_init(&available_sem, 0, 0) < 0) {
		perror("sem_init: ");
		exit(EXIT_FAILURE);
	}

  if((available_files = malloc(1)) == NULL) {
    perror("malloc 'available_files': ");
    exit(EXIT_FAILURE);
  }

  if((total_sizes = malloc(1)) == NULL) {
    perror("malloc 'total_sizes': ");
    exit(EXIT_FAILURE);
  }

	if((done_threads = malloc(thread_max * sizeof(bool))) == NULL) {
		perror("malloc 'done_threads': ");
		exit(EXIT_FAILURE);
	}

	for(int i = 0; i < thread_max; i++) {
		done_threads[i] = false;
	}

	//Add given files to the array of files.
	int id = 0;
	for(int i = optind; argv[i] != NULL; i++) {

		struct dir_info file;

		if((file.name = malloc((strlen(argv[i]) + 1) * sizeof(char))) == NULL) {
			perror("malloc file.name");
			exit(EXIT_FAILURE);
		}

		if((total_sizes = realloc(total_sizes, (id + 1) * sizeof(int))) == NULL) {
			perror("malloc total_sizes: ");
			exit(EXIT_FAILURE);
		}
		total_sizes[id] = 0;

		strcpy(file.name, argv[i]);
		file.parent_id = id;
		initialize_files(file);
		id++;
	}
}

/*
*	Adds the directories given by the user to the array of files to be measured.
*
*	@file: The file to be measured.
*
*	Returns: Nothing.
*
*/
void initialize_files(struct dir_info file) {
  struct stat file_stat;

	if(lstat(file.name, &file_stat) < 0) {
		fprintf(stderr, "unable to stat: '%s': ", file.name);
		perror("");
		exit_status = 1;
	}

  total_sizes[file.parent_id] += file_stat.st_blocks;

	//If it's a directory add it to the array otherwise remove it since we
	//already have its size.
  if(is_dir(file_stat)) {
    set_available_file(file);
		sem_post(&available_sem);
  }
  else {
    free(file.name);
  }
}

/*
*	Prints the array of sizes for each given file.
*
*	@files: The array of files to have their sizes printed.
*
*	Returns: Nothing.
*
*/
void print(char **files) {
  int j = 0;
  for(int i = optind; files[i] != NULL; i++) {
    printf("%d\t%s\n", total_sizes[j] / 2, files[i]);
    j++;
  }
}

/*
*	Checks if given file is a directory.
*
*	@file_stat: The stat struct for the given file.
*
*	Returns: True if the file is a directory and false otherwise.
*
*/
bool is_dir(struct stat file_stat) {
	return S_ISDIR(file_stat.st_mode);
}

/*
*	Function to join all threads.
*
*	@threads: array of threads.
*	@thread_amount: Number of threads.
*
*	Returns: Nothing.
*
*/
void join_threads(pthread_t *threads, int thread_amount) {
	for(int i = 0; i < thread_amount; i++) {
		if(pthread_join(threads[i], NULL) != 0) {
			fprintf(stderr, "pthread_join thread nr %d\n", i);
		}
	}
}

/*
*	Destroys the mutexes and frees alloced global variables.
*
*	Returns: Nothing.
*
*/
void free_memory(void) {
  pthread_mutex_destroy(&status_lock);
  pthread_mutex_destroy(&size_lock);
  pthread_mutex_destroy(&available_lock);
	if(sem_destroy(&available_sem) < 0) {
		perror("sem_destroy: ");
		exit(EXIT_FAILURE);
	}

	free(done_threads);
  free(total_sizes);
  free(available_files);
}
