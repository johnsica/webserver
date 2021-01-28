#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

static void die(const char *s){
    perror(s);
    exit(1);
}



int main(int argc, char **argv){
    //program takes five parameters
    if(argc != 5){
        fprintf(stderr, "usage: %s <server-port> <web-root> <mdb-lookup-host> <mdb-lookup-port>\n", argv[0]);
        exit(1);
    }

    unsigned short port = atoi(argv[1]);
    char *webroot = argv[2];
    char *host = argv[3];
    unsigned short mdbport = atoi(argv[4]);
    char *hostIP;

    //convert host name into an IP address
    struct hostent *he;
    if((he = gethostbyname(host)) == NULL){
        die("gethostbyname failed");
    }
    hostIP = inet_ntoa(*(struct in_addr *)he->h_addr);

    int servsock;                   // server socket descriptor
    int clntsock;                   // client socket descriptor
    int mdbsock;                    // database socket descriptor
    struct sockaddr_in servaddr;    // local address
    struct sockaddr_in clntaddr;    // client address
    struct sockaddr_in mdbaddr;     // database address
    unsigned int clntlen;           // length of client addres data struct

    //create socket to connect with mdb
    if((mdbsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    //ignore SIGPIPE so that we dont terminate when we call send()
    //on a disconnected socket.
    if(signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");


    //create socket for incoming connections
    if((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    //construct local address structure
    memset(&servaddr, 0, sizeof(servaddr));         //zero out structure
    servaddr.sin_family = AF_INET;                  //internet address family
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);   //any network interface
    servaddr.sin_port = htons(port);                //local port

    //mdb-lookup-server structure
    memset(&mdbaddr, 0, sizeof(mdbaddr));
    mdbaddr.sin_family = AF_INET;
    mdbaddr.sin_addr.s_addr = inet_addr(hostIP);
    mdbaddr.sin_port = htons(mdbport);

    //Establish TCP connection with mdb-lookup-server
    if(connect(mdbsock, (struct sockaddr *)&mdbaddr, sizeof(mdbaddr)) < 0){
        die("connect failed");
    }

    //bind to the local address
    if(bind(servsock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        die("bind failed");

    //start listening for incoming connections
    //5 being the max queue
    if(listen(servsock, 5) < 0)
        die("listen failed");

    FILE *fp, *response;
    char buf[4096], error[4096];
    char out[4096], read[4096];
    struct stat path;
    while(1){
        char *ok = "HTTP/1.0 200 OK\r\n\r\n";
        char *status = "200 OK\n";
        const char *form =
                    "<html><body>\n"
                    "<h1>mdb-lookup</h1>\n"
                    "<p>\n"
                    "<form method=GET action=/mdb-lookup>\n"
                    "lookup: <input type=text name=key>\n"
                    "<input type=submit>\n"
                    "</form>\n"
                    "<p>\n"
                    "</body></html>\n";
                
        //accept an incoming connection
        clntlen = sizeof(clntaddr); // initialize the in-out parameter

        //wait for client to connect
        if((clntsock = accept(servsock, 
                        (struct sockaddr *) &clntaddr, &clntlen)) < 0)
            die("accept failed");
 
        //receive request
        int r = recv(clntsock, buf, sizeof(buf), 0);
        if(r < 0)
            die("recv failed");
        else if(r == 0)
            die("connection closed prematurely");

        //check request
        char *token_separators = "\t \r\n"; //tab, space, new line
        char *method = strtok(buf, token_separators);
        char *requestURI = strtok(NULL, token_separators);
        char *httpVersion = strtok(NULL, token_separators);

        //add requestURI to web root 
        char tmpRoot[strlen(webroot)];  
        strncpy(tmpRoot, webroot, strlen(webroot));        

        //only support GET method
        if(strncmp(method, "GET ", strlen(method)) == 0){
           //check that URI starts w/ '/'.
            if(requestURI[0] != '/'){
                //if not, respond with "400 Bad Request"
                status = "400 Bad Request\n";
                snprintf(error, sizeof(error),  "<html><body\n"
                                                "<h1>%s</h1>\n"
                                                "</body></html>\n", status);
                                                
                send(clntsock, error, strlen(error), 0);
                close(clntsock);
                
            }

            /*  make sure requestURI does not contain "/../" and
             * that it does not end with "/.." 
             * */ 
            if(strstr(requestURI, "/../") != NULL || 
                    strstr(requestURI, "/..") != NULL){
                status = "400 Bad Request";
                snprintf(error, sizeof(error),  "<html><body\n"
                                                "<h1>%s</h1>\n"
                                                "</body></html>\n", status);
                send(clntsock, error, strlen(error), 0);                
            }

            //accept either http version 1.0 or 1.1
            if((strncmp(httpVersion, "HTTP/1.0", strlen(httpVersion)) != 0) && 
                (strncmp(httpVersion, "HTTP/1.1", strlen(httpVersion)) != 0)){
                    //write 501 status code
                status = "501 Not Implemented";
                snprintf(error, sizeof(error),  "<html><body\n"
                                                "<h1>%s</h1>\n"
                                                "</body></html>\n", status);
                send(clntsock, error, strlen(error), 0);                
                close(clntsock);
            }

            if(strncmp(requestURI, "/mdb-lookup", strlen(requestURI)) == 0){
                send(clntsock, ok, strlen(ok), 0);
                //send form to client
                if(send(clntsock, form, strlen(form), 0) == strlen(form))
                    fprintf(stdout, "%s \"%s %s %s\" %s",inet_ntoa(clntaddr.sin_addr),
                            method,requestURI, httpVersion, status);                   

            }

            else if(strstr(requestURI, "/mdb-lookup?key=") != NULL){
                //make sure buffer is empty
                char output[4096];
                output[0] = '\0';

                char temp[4096];
                char *string, t[strlen(requestURI)];
             
                int i, counter = 0;
                t[strlen(requestURI)] = '\0';
                strncpy(t, requestURI, strlen(requestURI));
                
                //separate the string
                if((string = strrchr(t, '=')) != NULL){
                    string[strlen(string)] = '\0';
                    //remove '=' from string
                    for(i=0; i<strlen(string); i++){
                        string[i] = string[i+1];
                    } 
                    //append newline to search string
                    strcat(string, "\n");
                }
                //send string to mdb-lookup-server
                send(mdbsock, string, strlen(string), 0);
                if((response = fdopen(mdbsock, "rb")) == NULL)
                    die("error opening response\n");
                //send status OK to client
                send(clntsock, ok, strlen(ok), 0);

                //create table
                char *formIntro =   "<html><body>\n"
                                    "<h1>mdb-lookup</h1>\n"
                                    "<p>\n"
                                    "<form method=GET action=/mdb-lookup>\n"
                                    "lookup: <input type=text name=key>\n"
                                    "<input type=submit>\n"
                                    "</form>\n"
                                    "<p>\n"
                                    "<p><table border>\n";
                //send form intro
                if(send(clntsock, formIntro, strlen(formIntro), 0) != strlen(formIntro))
                    die("send content failed");
                while(fgets(output, sizeof(output), response) != NULL){
                    if(strncmp(output, "\n", sizeof(output)) == 0){
                        output[0] = 0;
                        break;
                    }
                    if((counter % 2) == 0){
                        snprintf(temp, sizeof(temp), "<tr><td> %s\n", output);
                        if(send(clntsock, temp, strlen(temp), 0) != strlen(temp))
                            die("send content failed");
                        }
                    else{
                        snprintf(temp,sizeof(temp),"<tr><td bgcolor=yellow> %s\n",output);
                        if(send(clntsock, temp, strlen(temp), 0) != strlen(temp))
                            die("send content failed");
                    }
                    ++counter;
                }
                char *end =   "</table>\n"
                                "</body></html>\n";
                //send closing HTML statement
                send(clntsock, end, strlen(end), 0);
                string[strlen(string)-1] = 0;
                if(counter > 0)
                    fprintf(stdout, "looking up [%s]: %s \"%s %s %s\" %s",
                        string, inet_ntoa(clntaddr.sin_addr), method, 
                        requestURI, httpVersion, status);   
                else{
                    status = "404 Not Found";
                    snprintf(temp, sizeof(temp),    "<html><body\n"
                                                    "<h1>%s</h1>\n"
                                                    "</body></html>\n", status);
                    send(clntsock, error, strlen(error), 0);
                    close(clntsock);                
                }
               
            }
            else{
                tmpRoot[strlen(webroot)] = '\0';   
        
                //concatenate temp root
                strcat(tmpRoot, requestURI);
                //determine if path is a file or directory
                if(stat(tmpRoot, &path) == 0){
                    //if path is a file:
                    if(S_ISREG(path.st_mode)){
                        //check last character in path
                        if(tmpRoot[strlen(tmpRoot)-1] == '/'){
                            //concatenate "index.html"
                            strcat(tmpRoot, "index.html");
                        }
                    }
                    //if path is directory
                    else if(S_ISDIR(path.st_mode)){
                        if(tmpRoot[strlen(tmpRoot)-1] != '/'){
                            strcat(tmpRoot, "/index.html");
                        }
                        else if(tmpRoot[strlen(tmpRoot)-1] == '/'){
                            strcat(tmpRoot, "index.html");
                        }
                    }
                }
                else{
                    fprintf(stderr, "stat() failed: ");
                    perror(0);
                }
              
                 //check for requested file 
                if((fp = fopen(tmpRoot, "rb")) != NULL){
                    //send HTTP/1.0 response
                    snprintf(out, sizeof(out), "HTTP/1.0 200 OK\r\n\r\n");
                    send(clntsock, out, strlen(out), 0);      

                    //read file
                    unsigned int r;
                    while((r = fread(read, 1, sizeof(read), fp)) >0){
                        send(clntsock, read, r, 0);
                    }     
                
                }
                else{
                    status = "404 Not Found";
                    snprintf(error, sizeof(error),  "<html><body\n"
                                                    "<h1>%s</h1>\n"
                                                    "</body></html>\n", status);
                    send(clntsock, error, strlen(error), 0);
                    close(clntsock);     
                }
                //log request
                fprintf(stdout, "%s \"%s %s %s\" %s", inet_ntoa(clntaddr.sin_addr), 
                    method, requestURI, httpVersion, status);
        
                fclose(fp);
            }   
           
        }

        //else if method != GET, 501
        //this should be done with all status codes except zero
        else{
            snprintf(error, sizeof(error), "HTTP/1.0 501 Not Implemented\r\n"
                    "<html><body><h1>501 Not Implemented</h1></body></html>");
            //send error message to client
            send(clntsock, error, strlen(error), 0); 

        }
        
        close(clntsock);
    }
    fclose(response);
    close(mdbsock);

    return 0;    

}