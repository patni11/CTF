#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include "sqlite3.h"

/*************************** USAGE NOTES *****************************

./expense --help

./expense --add "DESCRIPTION" AMOUNT
  Added item ID for USER

./expense --list
  ID     DATE/TIME     USER       DESCRIPTION     AMOUNT

./expense --del 4
  Deleted item ID for USER

./expense --admin [--db db_filename] --add USER "DESCRIPTION" AMOUNT
./expense --admin [--db db_filename] --list USER
./expense --admin [--db db_filename] --del USER "DESCRIPTION" AMOUNT
./expense --admin [--db db_filename] --listusers

***********************************************************************/

#define ADMIN_PW       "" // ADMIN PASSWORD REDACTED TO PRESERVE CONFIDENTIALITY!!!
#define DB_FILENAME    "expenses.sqlite"
#define LIST_USER_SQL  "SELECT DISTINCT username FROM expenses"
#define ADD_EXP_SQL    "INSERT INTO expenses (time, username, description, amount) VALUES (%li, \"%s\", \"%s\", %s)"
#define DEL_EXP_SQL    "DELETE FROM expenses WHERE username=\"%s\" AND id=%s"
#define LIST_EXP_SQL   "SELECT * FROM expenses WHERE username=\"%s\""
#define TEST_EXP_SQL   "SELECT * FROM expenses"
#define AUDIT_LOG_SQL  "INSERT INTO audit_log (time, admin, username, command, arguments) VALUES (%li, %i, \"%s\", \"%s\", \"%s\")"
#define MAX_QUERY_LEN  2048
#define MAX_TIME_LEN   20  // max character length of an unsigned long int

// Check if the help message is being requested, or the user hasn't typed any command line args
//   argc : The total number of command line arguments
//   argv : Array of strings, each containing a command line argument
// Exits the program if the help message is displayed
void help(int argc, char ** argv) {
    int i;
    int found = 0;

    if (argc == 1) found = 1;

    for (i = 1; i < argc; ++i) if (0 == strcmp(argv[i], "--help")) {
        found = 1;
        break;
    }

    if (found) {
        puts(
            "This program allows users to track their expenses. It has commands for adding, viewing, and deleting\n" \
            "expenses. Administrators may add, view, or delete expenses for any user. The program also keeps a log\n" \
            "of all changes to facilitate offline auditing.\n"
        );
        printf("Usage: %s [--admin] <--command> [arguments]\n", argv[0]);
        puts(
            "  Commands:\n" \
            "    --help                           Displays this message\n" \
            "    --add <Description> <Amount>     Adds an expense for the current user\n" \
            "    --list                           Lists all expenses for the current user\n" \
            "    --del <ID>                       Deletes the current user's expense with the given ID\n" \
            "\n" \
            "Additional commands are available to administrators. See the developer docs for more information."
        );

        exit(0);
    }
}

// Asks the user to input the admin password and checks the result
// Returns 1 on success, 0 on failure
int authenticate() {
    char buf[256];
    printf("Enter the administrator password: ");
    gets(buf);

    // Is the password given by the user correct?
    if (0 == strcmp(buf, ADMIN_PW)) return 1;
    return 0;
}

// Prints an error message and exits the program 
//   msg : error message to print
// Exits the program
void error(char * msg) {
    printf("Error: %s\n", msg);
    exit(-1);
}

// Prints an error message from the sqlite3 database and exits the program 
//    db : the sqlite3 database
//   msg : error message to print (malloced by sqlite3)
// Exits the program
void sql_error(sqlite3 * db, char * msg) {
    sqlite3_close(db);
    printf("Error from sqlite: %s\n", msg);
    sqlite3_free(msg);
    exit(-1);
}

// Attempts to open the sqlite3 database
//   filename : the path and filename of the sqlite3 database to open
// Returns a pointer to the open database
// Exits the program if the file cannot be found, or it is not a valid sqlite3 database
sqlite3 * open_db(char * filename) {
    sqlite3 * db;
    FILE * file;
    char buf[256], * err;

    // Make sure the specified file exists
    if (-1 == access(filename, F_OK))
        error("cannot open the specified database file");

    // Try to open the database
    int result = sqlite3_open(filename, &db);
    if (result != SQLITE_OK)
        error("cannot open the specified database file");

    // Run a test query
    result = sqlite3_exec(db, TEST_EXP_SQL, 0, 0, &err);
    if (result != SQLITE_OK) {
        sqlite3_close(db);

        // Is the database corrupt or non-existant?
        file = fopen(filename, "r");
        if (!file) error("cannot open the specified database file");

        // The database is corrupt; try printing out the contents to help the administrator
        printf("%s does not appear to be a valid sqlite3 database file.\n", filename);
        puts("To aid in debugging, here are the contents of the specified file:");
        while (!feof(file)) {
            fgets(buf, 255, file);
            fputs(buf, stdout);
        }
        // Don't forget to close the file
        fclose(file);
        exit(-1);
    }
    return db;
}

// Logs --add and --del commands in the audit_log table
//       db : the sqlite3 database
//        t : the current time
//    admin : 0 if the user is not authenticated as admin, 1 otherwise
// username : the username of the user who was targeted by the command (may not be the user who ran the program)
//      cmd : the command that was executed, either "add" or "del"
//     args : the arguments that were given to the command
void audit_log(sqlite3 * db, time_t t, int admin, char * username, char * cmd, char * args) {
    char buf[MAX_QUERY_LEN];
    char * err;
    int result;

    // Do we have enough space to build the query string?
    if (strlen(AUDIT_LOG_SQL) + MAX_TIME_LEN + 1 + strlen(username) + strlen(cmd) + strlen(args) >= MAX_QUERY_LEN - 1)
        error("sorry, the audit log entry is too long");

    // Build the query
    sprintf(buf, AUDIT_LOG_SQL, t, admin, username, cmd, args);
    
    // Add the entry to the log
    result = sqlite3_exec(db, buf, 0, 0, &err);
    if (result != SQLITE_OK) sql_error(db, err);
}

// Handles the --add command, adds an expense from the database and logs it
//       db : the sqlite3 database
//        i : the index of the current command line argument to parse
//     argc : the total number of command line arguments
//     argv : array of strings, each containing a command line argument
// username : the username of the user who ran the program
//    admin : 0 if the user is not authenticated as admin, 1 otherwise
void add_expense(sqlite3 * db, int i, int argc, char ** argv, char * username, int admin) {
    char buf[MAX_QUERY_LEN];
    char * desc, * amount, * err;
    int result;
    time_t t = time(0);

    // If the user is not admin, get the description and amount of the new expense
    if (!admin) {
        if (i + 1 >= argc) error("insufficient arguments for the --add command");
        desc = argv[i];
        amount = argv[i+1];
    }
    // If the user is admin, get the username, description, and amount of the new expense
    else {
        if (i + 2 >= argc) error("insufficient arguments for the --add command");
        username = argv[i];
        desc = argv[i+1];
        amount = argv[i+2];
    }

    // Do we have enough space to build the query string?
    if (strlen(ADD_EXP_SQL) + MAX_TIME_LEN + strlen(username) + strlen(desc) + strlen(amount) >= MAX_QUERY_LEN - 1)
        error("sorry, the length of your expense is too long");

    // Build the query string
    sprintf(buf, ADD_EXP_SQL, t, username, desc, amount);
    
    // Run the query
    result = sqlite3_exec(db, buf, 0, 0, &err);
    if (result != SQLITE_OK) sql_error(db, err);

    // Log the add
    sprintf(buf, "%s %s", desc, amount);
    audit_log(db, t, admin, username, "add", buf);
}

// Handles the --list command, lists all the expenses for a given user
//       db : the sqlite3 database
//        i : the index of the current command line argument to parse
//     argc : the total number of command line arguments
//     argv : array of strings, each containing a command line argument
// username : the username of the user who ran the program
//    admin : 0 if the user is not authenticated as admin, 1 otherwise
void list_expenses(sqlite3 * db, int i, int argc, char ** argv, char * username, int admin) {
    int result;
    char buf[MAX_QUERY_LEN];
    sqlite3_stmt * stmt;
    time_t t;
    char * ts;

    // If the user is admin, get the user that should be listed
    if (admin) {
        if (i >= argc) error("insufficient arguments for the --list command");
        username = argv[i];
    }

    // Do we have enough space to construct the query string?
    if (strlen(LIST_EXP_SQL) + strlen(username) >= MAX_QUERY_LEN - 1)
        error("sorry, the length of your username is too long");

    // Build the query string
    sprintf(buf, LIST_EXP_SQL, username);

    // Run the query
    result = sqlite3_prepare_v2(db, buf, strlen(buf)+1, &stmt, NULL);
    if (result != SQLITE_OK) {
        sqlite3_close(db);
        error("unable to execute --list query");
    }

    // Iterate through the result rows and print them
    printf("%5s %24s %16s %40s %10s\n", "ID", "Date/Time", "User", "Description", "Amount");
    do {
        result = sqlite3_step(stmt);
        if (result == SQLITE_ROW) {
            // Convert the integer time to a human-readable string
            t = sqlite3_column_int(stmt, 1);
            ts = ctime(&t);
            ts[strlen(ts) - 2] = 0; // remove annoying \n

            printf("%5i %24s %16s %40s %10.2f\n",
                sqlite3_column_int(stmt, 0),
                ts,
                (char *) sqlite3_column_text(stmt, 2),
                (char *) sqlite3_column_text(stmt, 3),
                sqlite3_column_double(stmt, 4)
            );
        }
    } while (result == SQLITE_ROW);
}

// Handles the --del command, deletes an expense from the database and logs it
//       db : the sqlite3 database
//        i : the index of the current command line argument to parse
//     argc : the total number of command line arguments
//     argv : array of strings, each containing a command line argument
// username : the username of the user who ran the program
//    admin : 0 if the user is not authenticated as admin, 1 otherwise
void del_expense(sqlite3 * db, int i, int argc, char ** argv, char * username, int admin) {
    char buf[MAX_QUERY_LEN];
    char * id, * err;
    int result;
    time_t t = time(0);

    // If the user is not admin, get the id that should be deleted
    if (!admin) {
        if (i >= argc) error("insufficient arguments for the --del command");
        id = argv[i];
    }
    // If the user is admin, get the username and the id that should be deleted
    else {
        if (i + 1 >= argc) error("insufficient arguments for the --del command");
        username = argv[i];
        id = argv[i+1];
    }

    // Do we have enough space to construct the query string?
    if (strlen(DEL_EXP_SQL) + strlen(username) + strlen(id) >= MAX_QUERY_LEN - 1)
        error("sorry, the length of your expense (username + description + amount) is too long");

    // Build the query
    sprintf(buf, DEL_EXP_SQL, username, id);
    
    // Run the query
    result = sqlite3_exec(db, buf, 0, 0, &err);
    if (result != SQLITE_OK) sql_error(db, err);

    // Log the delete
    sprintf(buf, "%s", id);
    audit_log(db, t, admin, username, "del", buf);
}

// Handles the --listusers command, prints all users in the database who have expenses
//   db    : the sqlite3 database
//   admin : 0 if the user is not authenticated as admin, 1 otherwise
void list_users(sqlite3 * db, int admin) {
    int result;
    sqlite3_stmt * stmt;

    // Make sure the user is authenticated for this privileged command
    if (!admin) error("you must have administrator access to run this command");

    // Query the database
    result = sqlite3_prepare_v2(db, LIST_USER_SQL, strlen(LIST_USER_SQL)+1, &stmt, NULL);
    if (result != SQLITE_OK) {
        sqlite3_close(db);
        error("unable to execute --listusers query");
    }

    // Iterate through the resulting rows and print them
    do {
        result = sqlite3_step(stmt);
        if (result == SQLITE_ROW) puts((char *) sqlite3_column_text(stmt, 0));
    } while (result == SQLITE_ROW);
}

// Program Start Here
//   argc : The total number of command line arguments
//   argv : Array of strings, each containing a command line argument
int main(int argc, char** argv) {
    int admin = 0;
    int i = 1;
    char * db_filename = DB_FILENAME;
    struct passwd *p;
    char * username;
    sqlite3 * db;

    // Print the help message, if requested or too few args given
    help(argc, argv);

    // Get the username of the user who ran the program
    p = getpwuid(getuid());
    username = p->pw_name;

    // Upgrade to setuid privileges
    setuid(geteuid());

    // If the user is requesting admin rights, authenticate them first
    if (0 == strcmp(argv[i], "--admin")) {
        if (!authenticate()) error("incorrect password");
        i = 2;
        admin = 1;
    }

    // Is the user trying to load a non-default database file?
    if (i < argc && 0 == strcmp(argv[i], "--db")) {
        if (!admin) error("only administrators may use the --db command");
        i += 1;
        if (i == argc) error("insufficient arguments supplied for --db command");
        db_filename = argv[i];
        i += 1;
    }

    // Execute the users command, if one is given
    if (i < argc) {
        // But first, open the database
        db = open_db(db_filename);

        if (0 == strcmp(argv[i], "--add")) add_expense(db, i+1, argc, argv, username, admin);
        else if (0 == strcmp(argv[i], "--list")) list_expenses(db, i+1, argc, argv, username, admin);
        else if (0 == strcmp(argv[i], "--del")) del_expense(db, i+1, argc, argv, username, admin);
        else if (0 == strcmp(argv[i], "--listusers")) list_users(db, admin);
        else error("unknown command supplied");

        // Don't forget to close the database
        sqlite3_close(db);
    }
    else error("no command supplied");

    return 0;
}