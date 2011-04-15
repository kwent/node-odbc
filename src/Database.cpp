/*
  Copyright (c) 2010, Lee Smith<notwink@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <string.h>
#include <v8.h>
#include <node.h>
#include <node_events.h>
#include <time.h>

#include "Database.h"

#define MAX_FIELD_SIZE 1024
#define MAX_VALUE_SIZE 1048576

using namespace v8;
using namespace node;

typedef struct {
  unsigned char *name;
  unsigned int len;
  SQLLEN type;
} Column;


void Database::Init(v8::Handle<Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);

  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->Inherit(EventEmitter::constructor_template);
  constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
  constructor_template->SetClassName(String::NewSymbol("Database"));

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "open", Open);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "close", Close);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "dispatchQuery", Query);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "dispatchTables", Tables);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "dispatchColumns", Columns);

  target->Set(v8::String::NewSymbol("Database"), constructor_template->GetFunction());
}

Handle<Value> Database::New(const Arguments& args) {
  HandleScope scope;
  Database* dbo = new Database();
  dbo->Wrap(args.This());
  return args.This();
}

int Database::EIO_AfterOpen(eio_req *req) {
  ev_unref(EV_DEFAULT_UC);
  HandleScope scope;
  struct open_request *open_req = (struct open_request *)(req->data);

  Local<Value> argv[1];
  bool err = false;
  if (req->result) {
    err = true;
    argv[0] = Exception::Error(String::New("Error opening database"));
  }

  TryCatch try_catch;

  open_req->dbo->Unref();
  open_req->cb->Call(Context::GetCurrent()->Global(), err ? 1 : 0, argv);

  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }

  open_req->dbo->Emit(String::New("ready"), 0, NULL);
  open_req->cb.Dispose();

  free(open_req);
  scope.Close(Undefined());
  return 0;
}

int Database::EIO_Open(eio_req *req) {
  struct open_request *open_req = (struct open_request *)(req->data);
  Database *self = open_req->dbo->self();

  int ret = SQLAllocEnv( &self->m_hEnv );
  if( ret == SQL_SUCCESS ) {
    ret = SQLAllocConnect( self->m_hEnv,&self->m_hDBC );
    if( ret == SQL_SUCCESS ) {
      SQLSetConnectOption( self->m_hDBC,SQL_LOGIN_TIMEOUT,5 );
      char connstr[1024];
      ret = SQLDriverConnect(self->m_hDBC,NULL,(SQLCHAR*)open_req->connection,strlen(open_req->connection),(SQLCHAR*)connstr,1024,NULL,SQL_DRIVER_NOPROMPT);

      if( ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO )
        {
          ret = SQLAllocStmt( self->m_hDBC,&self->m_hStmt );
          if (ret != SQL_SUCCESS) printf("not connected\n");
          
          if ( !SQL_SUCCEEDED( SQLGetFunctions(self->m_hDBC, SQL_API_SQLMORERESULTS, &self->canHaveMoreResults)))
          {
            self->canHaveMoreResults = 0;
          }
        }
      else
        {
          self->printError("SQLDriverConnect", self->m_hDBC, SQL_HANDLE_DBC);
        }
    }
  }
  req->result = ret;
  return 0;
}

Handle<Value> Database::Open(const Arguments& args) {
  HandleScope scope;

  REQ_STR_ARG(0, connection);
  REQ_FUN_ARG(1, cb);

  Database* dbo = ObjectWrap::Unwrap<Database>(args.This());

  struct open_request *open_req = (struct open_request *)
    calloc(1, sizeof(struct open_request) + connection.length());

  if (!open_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  strcpy(open_req->connection, *connection);
  open_req->cb = Persistent<Function>::New(cb);
  open_req->dbo = dbo;

  eio_custom(EIO_Open, EIO_PRI_DEFAULT, EIO_AfterOpen, open_req);

  ev_ref(EV_DEFAULT_UC);
  dbo->Ref();
  scope.Close(Undefined());
  return Undefined();
}

int Database::EIO_AfterClose(eio_req *req) {
  ev_unref(EV_DEFAULT_UC);

  HandleScope scope;

  struct close_request *close_req = (struct close_request *)(req->data);

  Local<Value> argv[1];
  bool err = false;
  if (req->result) {
    err = true;
    argv[0] = Exception::Error(String::New("Error closing database"));
  }

  TryCatch try_catch;

  close_req->dbo->Unref();
  close_req->cb->Call(Context::GetCurrent()->Global(), err ? 1 : 0, argv);

  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }

  close_req->cb.Dispose();

  free(close_req);
  scope.Close(Undefined());
  return 0;
}

int Database::EIO_Close(eio_req *req) {
  struct close_request *close_req = (struct close_request *)(req->data);
  Database* dbo = close_req->dbo;
  SQLDisconnect(dbo->m_hDBC);
  SQLFreeHandle(SQL_HANDLE_ENV, dbo->m_hEnv);
  SQLFreeHandle(SQL_HANDLE_DBC, dbo->m_hDBC);
  return 0;
}

Handle<Value> Database::Close(const Arguments& args) {
  HandleScope scope;

  REQ_FUN_ARG(0, cb);

  Database* dbo = ObjectWrap::Unwrap<Database>(args.This());

  struct close_request *close_req = (struct close_request *)
    calloc(1, sizeof(struct close_request));

  if (!close_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  close_req->cb = Persistent<Function>::New(cb);
  close_req->dbo = dbo;

  eio_custom(EIO_Close, EIO_PRI_DEFAULT, EIO_AfterClose, close_req);

  ev_ref(EV_DEFAULT_UC);
  dbo->Ref();
  scope.Close(Undefined());
  return Undefined();
}

int Database::EIO_AfterQuery(eio_req *req) {
  //printf("Database::EIO_AfterQuery\n");
  ev_unref(EV_DEFAULT_UC);

  struct query_request *prep_req = (struct query_request *)(req->data);
  
  HandleScope scope;
  
  Database *self = prep_req->dbo->self(); //an easy reference to the Database object
  Local<Object> objError = Object::New(); //our error object which we will use if we discover errors while processing the result set
  
  short colCount = 0; //used to keep track of the number of columns received in a result set
  short emitCount = 0; //used to keep track of the number of event emittions that have occurred
  short errorCount = 0; //used to keep track of the number of errors that have been found
  
  SQLSMALLINT buflen; //used as a place holder for the length of column names
  SQLRETURN ret; //used to capture the return value from various SQL function calls
  
  char *buf = (char *) malloc(MAX_VALUE_SIZE); //allocate a buffer for incoming column values
  
  //check to make sure malloc succeeded
  if (buf == NULL) {
    //malloc failed, set an error message
    objError->Set(String::New("error"), String::New("[node-odbc] Failed Malloc"));
    objError->Set(String::New("message"), String::New("An attempt to allocate memory failed. This allocation was for a value buffer of incoming recordset values."));
    
    //emit an error event immidiately.
    Local<Value> args[3];
    args[0] = objError;
    args[1] = Local<Value>::New(Null());
    args[2] = Local<Boolean>::New(False());
    
    //emit an error event
    self->Emit(String::New("error"), 3, args);
    
    //emit a result event
    self->Emit(String::New("result"), 3, args);
  }
  else {
    //malloc succeeded so let's continue -- I'm not too fond of having all this code in the else statement, but I don't know what else to do...
    
    memset(buf,0,MAX_VALUE_SIZE); //set all of the bytes of the buffer to 0; I tried doing this inside the loop, but it increased processing time dramatically
    struct tm timeInfo = { 0 }; //used for processing date/time datatypes 
    
    //First thing, let's check if the execution of the query returned any errors (in EIO_Query)
    if(req->result == SQL_ERROR)
    {
      errorCount++;
      
      char errorMessage[512];
      char errorSQLState[128];
      SQLError(self->m_hEnv, self->m_hDBC, self->m_hStmt,(SQLCHAR *)errorSQLState,NULL,(SQLCHAR *)errorMessage, sizeof(errorMessage), NULL);
      objError->Set(String::New("state"), String::New(errorSQLState));
      objError->Set(String::New("error"), String::New(errorMessage));
      
      //only set the query value of the object if we actually have a query
      if (prep_req->sql != NULL) {
        objError->Set(String::New("query"), String::New(prep_req->sql));
      }
      
      //emit an error event immidiately.
      Local<Value> args[1];
      args[0] = objError;
      self->Emit(String::New("error"), 1, args);
    }
    
    //loop through all result sets
    do {
      colCount = 0; //always reset colCount for the current result set to 0;
      
      SQLNumResultCols(self->m_hStmt, &colCount);
      Column *columns = new Column[colCount];
      
      Local<Array> rows = Array::New();
      
      if (colCount > 0) {
        // retrieve and store column attributes to build the row object
        for(int i = 0; i < colCount; i++)
        {
          columns[i].name = new unsigned char[MAX_FIELD_SIZE];
          
          //zero out the space where the column name will be stored
          memset(columns[i].name, 0, MAX_FIELD_SIZE);
          
          //get the column name
          ret = SQLColAttribute(self->m_hStmt, (SQLUSMALLINT)i+1, SQL_DESC_LABEL, columns[i].name, (SQLSMALLINT)MAX_FIELD_SIZE, (SQLSMALLINT *)&buflen, NULL);
          
          //store the len attribute
          columns[i].len = buflen;
          
          //get the column type and store it directly in column[i].type
          ret = SQLColAttribute( self->m_hStmt, (SQLUSMALLINT)i+1, SQL_COLUMN_TYPE, NULL, NULL, NULL, &columns[i].type );
        }
        
        int count = 0;
        
        // i dont think odbc will tell how many rows are returned, loop until out...
        while(true)
        {
          Local<Object> tuple = Object::New();
          ret = SQLFetch(self->m_hStmt);
          
          //TODO: Do something to enable/disable dumping these info messages to the console.
          if (ret == SQL_SUCCESS_WITH_INFO ) {
            char errorMessage[512];
            char errorSQLState[128];
            SQLError(self->m_hEnv, self->m_hDBC, self->m_hStmt,(SQLCHAR *)errorSQLState,NULL,(SQLCHAR *)errorMessage, sizeof(errorMessage), NULL);
            
            //printf("EIO_Query ret => %i\n", ret);
            printf("EIO_Query => %s\n", errorMessage);
            printf("EIO_Query => %s\n", errorSQLState);
            //printf("EIO_Query sql => %s\n", prep_req->sql);
          }
          
          if (ret == SQL_ERROR)  {
            char errorMessage[512];
            char errorSQLState[128];
            SQLError(self->m_hEnv, self->m_hDBC, self->m_hStmt,(SQLCHAR *)errorSQLState,NULL,(SQLCHAR *)errorMessage, sizeof(errorMessage), NULL);
            
            errorCount++;
            objError->Set(String::New("state"), String::New(errorSQLState));
            objError->Set(String::New("error"), String::New(errorMessage));
            objError->Set(String::New("query"), String::New(prep_req->sql));
            
            //emit an error event immidiately.
            Local<Value> args[1];
            args[0] = objError;
            self->Emit(String::New("error"), 1, args);
            
            break;
          }
          
          if (ret == SQL_NO_DATA) {
            break;
          }
          
          for(int i = 0; i < colCount; i++)
          {
            SQLLEN len;
            
            // SQLGetData can supposedly return multiple chunks, need to do this to retrieve large fields
            int ret = SQLGetData(self->m_hStmt, i+1, SQL_CHAR, (char *) buf, MAX_VALUE_SIZE-1, (SQLLEN *) &len);
            
            //printf("%s %i\n", columns[i].name, columns[i].type);
            
            if(ret == SQL_NULL_DATA || len < 0)
            {
              tuple->Set(String::New((const char *)columns[i].name), Null());
            }
            else
            { 
              switch (columns[i].type) {
                case SQL_NUMERIC :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_DECIMAL :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_INTEGER :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_SMALLINT :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_FLOAT :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_REAL :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_DOUBLE :
                  tuple->Set(String::New((const char *)columns[i].name), Number::New(atof(buf)));
                  break;
                case SQL_DATETIME :
                case SQL_TIMESTAMP :
                  //I am not sure if this is locale-safe or cross database safe, but it works for me on MSSQL
                  strptime(buf, "%Y-%m-%d %H:%M:%S", &timeInfo);
                  timeInfo.tm_isdst = -1; //a negative value means that mktime() should (use timezone information and system 
                        //databases to) attempt to determine whether DST is in effect at the specified time.
                  
                  tuple->Set(String::New((const char *)columns[i].name), Date::New(mktime(&timeInfo) * 1000));
                  
                  break;
                case SQL_BIT :
                  //again, i'm not sure if this is cross database safe, but it works for MSSQL
                  tuple->Set(String::New((const char *)columns[i].name), Boolean::New( ( *buf == '0') ? false : true ));
                  break;
                default :
                  tuple->Set(String::New((const char *)columns[i].name), String::New(buf));
                  break;
              }
            }
          }
          
          rows->Set(Integer::New(count), tuple);
          count++;
        }
        
        for(int i = 0; i < colCount; i++)
        {
          delete [] columns[i].name;
        }

        delete [] columns;
      }
      
      //move to the next result set
      ret = SQLMoreResults( self->m_hStmt );
      
      if ( ret != SQL_SUCCESS ) {
        //there are no more recordsets so free the statement now before we emit
        //because as soon as we emit the last recordest, we are clear to submit another query
        //which could cause a race condition with freeing and allocating handles.
        SQLFreeStmt( self->m_hStmt, NULL );
        SQLAllocHandle( SQL_HANDLE_STMT, self->m_hDBC, &self->m_hStmt );
      }
      
      //Only trigger an emit if there are columns OR if this is the last result and none others have been emitted
      //odbc will process individual statments like select @something = 1 as a recordset even though it doesn't have
      //any columns. We don't want to emit those unless there are actually columns
      if (colCount > 0 || ( ret != SQL_SUCCESS && emitCount == 0 )) {
        emitCount++;
        
        Local<Value> args[3];
        
        if (errorCount) {
          args[0] = objError;
        }
        else {
          args[0] = Local<Value>::New(Null());
        }
        
        args[1] = rows;
        args[2] = Local<Boolean>::New(( ret == SQL_SUCCESS ) ? True() : False() ); //true or false, are there more result sets to follow this emit?
        
        self->Emit(String::New("result"), 3, args);
      }
    }
    while ( self->canHaveMoreResults && ret == SQL_SUCCESS );
  } //end of malloc check
  
  TryCatch try_catch;
  
  self->Unref();
  
  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }
  
  free(buf);
  free(prep_req->sql);
  free(prep_req->catalog);
  free(prep_req->schema);
  free(prep_req->table);
  free(prep_req->type);
  free(prep_req);
  scope.Close(Undefined());
  
  return 0;
}

int Database::EIO_Query(eio_req *req) {
  struct query_request *prep_req = (struct query_request *)(req->data);
  
  if(prep_req->dbo->m_hStmt)
  {
    SQLFreeStmt(prep_req->dbo->m_hStmt,NULL);
    SQLAllocStmt(prep_req->dbo->m_hDBC,&prep_req->dbo->m_hStmt );
  }
  
  SQLRETURN ret = SQLExecDirect( prep_req->dbo->m_hStmt,(SQLCHAR *)prep_req->sql, strlen(prep_req->sql) );
  
  req->result = ret; // this will be checked later in EIO_AfterQuery
  
  return 0;
}

Handle<Value> Database::Query(const Arguments& args) {
  HandleScope scope;

  REQ_STR_ARG(0, sql);
  //REQ_FUN_ARG(1, cb);

  Database* dbo = ObjectWrap::Unwrap<Database>(args.This());

  struct query_request *prep_req = (struct query_request *)
    calloc(1, sizeof(struct query_request));

  if (!prep_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  prep_req->sql = (char *) malloc(sql.length() +1);
  prep_req->catalog = NULL;
  prep_req->schema = NULL;
  prep_req->table = NULL;
  prep_req->type = NULL;
  prep_req->column = NULL;
  
  strcpy(prep_req->sql, *sql);
  
  prep_req->dbo = dbo;

  eio_custom(EIO_Query, EIO_PRI_DEFAULT, EIO_AfterQuery, prep_req);

  ev_ref(EV_DEFAULT_UC);
  dbo->Ref();
  scope.Close(Undefined());
  return Undefined();
}

int Database::EIO_Tables(eio_req *req) {
  struct query_request *prep_req = (struct query_request *)(req->data);
  
  if(prep_req->dbo->m_hStmt)
  {
    SQLFreeStmt(prep_req->dbo->m_hStmt,NULL);
    SQLAllocStmt(prep_req->dbo->m_hDBC,&prep_req->dbo->m_hStmt );
  }
  
  SQLRETURN ret = SQLTables( 
    prep_req->dbo->m_hStmt, 
    (SQLCHAR *) prep_req->catalog,   SQL_NTS, 
    (SQLCHAR *) prep_req->schema,   SQL_NTS, 
    (SQLCHAR *) prep_req->table,   SQL_NTS, 
    (SQLCHAR *) prep_req->type,   SQL_NTS
  );
  
  req->result = ret; // this will be checked later in EIO_AfterQuery
  
  return 0;
}

Handle<Value> Database::Tables(const Arguments& args) {
  //printf("Database::Tables\n");
  HandleScope scope;

  REQ_STR_OR_NULL_ARG(0, catalog);
  REQ_STR_OR_NULL_ARG(1, schema);
  REQ_STR_OR_NULL_ARG(2, table);
  REQ_STR_OR_NULL_ARG(3, type);
  
  Database* dbo = ObjectWrap::Unwrap<Database>(args.This());

  struct query_request *prep_req = (struct query_request *)
    calloc(1, sizeof(struct query_request));
  
  if (!prep_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  prep_req->sql = NULL;
  prep_req->catalog = NULL;
  prep_req->schema = NULL;
  prep_req->table = NULL;
  prep_req->type = NULL;
  prep_req->column = NULL;

  if (!String::New(*catalog)->Equals(String::New("null"))) {
    prep_req->catalog = (char *) malloc(catalog.length() +1);
    strcpy(prep_req->catalog, *catalog);
  }
  
  if (!String::New(*schema)->Equals(String::New("null"))) {
    prep_req->schema = (char *) malloc(schema.length() +1);
    strcpy(prep_req->schema, *schema);
  }
  
  if (!String::New(*table)->Equals(String::New("null"))) {
    prep_req->table = (char *) malloc(table.length() +1);
    strcpy(prep_req->table, *table);
  }
  
  if (!String::New(*type)->Equals(String::New("null"))) {
    prep_req->type = (char *) malloc(type.length() +1);
    strcpy(prep_req->type, *type);
  }
  
  prep_req->dbo = dbo;

  eio_custom(EIO_Tables, EIO_PRI_DEFAULT, EIO_AfterQuery, prep_req);

  ev_ref(EV_DEFAULT_UC);
  dbo->Ref();
  scope.Close(Undefined());
  return Undefined();
}

int Database::EIO_Columns(eio_req *req) {
  //printf("Database::EIO_Columns\n");
  struct query_request *prep_req = (struct query_request *)(req->data);
  
  if(prep_req->dbo->m_hStmt)
  {
    SQLFreeStmt(prep_req->dbo->m_hStmt,NULL);
    SQLAllocStmt(prep_req->dbo->m_hDBC,&prep_req->dbo->m_hStmt );
  }
  
  SQLRETURN ret = SQLColumns( 
    prep_req->dbo->m_hStmt, 
    (SQLCHAR *) prep_req->catalog,   SQL_NTS, 
    (SQLCHAR *) prep_req->schema,   SQL_NTS, 
    (SQLCHAR *) prep_req->table,   SQL_NTS, 
    (SQLCHAR *) prep_req->column,   SQL_NTS
  );
  
  req->result = ret; // this will be checked later in EIO_AfterQuery
  
  return 0;
}

Handle<Value> Database::Columns(const Arguments& args) {
  //printf("Database::Columns\n");
  HandleScope scope;

  REQ_STR_OR_NULL_ARG(0, catalog);
  REQ_STR_OR_NULL_ARG(1, schema);
  REQ_STR_OR_NULL_ARG(2, table);
  REQ_STR_OR_NULL_ARG(3, column);
  
  Database* dbo = ObjectWrap::Unwrap<Database>(args.This());

  struct query_request *prep_req = (struct query_request *)
    calloc(1, sizeof(struct query_request));
  
  if (!prep_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  prep_req->sql = NULL;
  prep_req->catalog = NULL;
  prep_req->schema = NULL;
  prep_req->table = NULL;
  prep_req->type = NULL;
  prep_req->column = NULL;

  if (!String::New(*catalog)->Equals(String::New("null"))) {
    prep_req->catalog = (char *) malloc(catalog.length() +1);
    strcpy(prep_req->catalog, *catalog);
  }
  
  if (!String::New(*schema)->Equals(String::New("null"))) {
    prep_req->schema = (char *) malloc(schema.length() +1);
    strcpy(prep_req->schema, *schema);
  }
  
  if (!String::New(*table)->Equals(String::New("null"))) {
    prep_req->table = (char *) malloc(table.length() +1);
    strcpy(prep_req->table, *table);
  }
  
  if (!String::New(*column)->Equals(String::New("null"))) {
    prep_req->column = (char *) malloc(column.length() +1);
    strcpy(prep_req->column, *column);
  }
  
  prep_req->dbo = dbo;

  eio_custom(EIO_Columns, EIO_PRI_DEFAULT, EIO_AfterQuery, prep_req);

  ev_ref(EV_DEFAULT_UC);
  dbo->Ref();
  scope.Close(Undefined());
  return Undefined();
}

void Database::printError(const char *fn, SQLHANDLE handle, SQLSMALLINT type)
{
  SQLINTEGER i = 0;
  SQLINTEGER native;
  SQLCHAR state[ 7 ];
  SQLCHAR text[256];
  SQLSMALLINT len;
  SQLRETURN ret;

  fprintf(stderr,
    "\n"
    "The driver reported the following diagnostics whilst running "
    "%s\n\n",
    fn
  );

  do {
    ret = SQLGetDiagRec(type, handle, ++i, state, &native, text, sizeof(text), &len );
    if (SQL_SUCCEEDED(ret))
      printf("%s:%ld:%ld:%s\n", state, (long int) i, (long int) native, text);
  }
  while( ret == SQL_SUCCESS );
}


Persistent<FunctionTemplate> Database::constructor_template;

extern "C" void init (v8::Handle<Object> target) {
  Database::Init(target);
}