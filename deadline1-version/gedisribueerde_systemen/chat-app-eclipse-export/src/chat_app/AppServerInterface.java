/**
 * Autogenerated by Avro
 * 
 * DO NOT EDIT DIRECTLY
 */
package chat_app;

@SuppressWarnings("all")
@org.apache.avro.specific.AvroGenerated
public interface AppServerInterface {
  public static final org.apache.avro.Protocol PROTOCOL = org.apache.avro.Protocol.parse("{\"protocol\":\"AppServerInterface\",\"namespace\":\"chat_app\",\"types\":[],\"messages\":{\"registerClient\":{\"request\":[{\"name\":\"username\",\"type\":\"string\"},{\"name\":\"ipaddress\",\"type\":\"string\"},{\"name\":\"port\",\"type\":\"int\"}],\"response\":\"int\"},\"exitClient\":{\"request\":[{\"name\":\"id\",\"type\":\"int\"}],\"response\":\"int\"},\"getListOfClients\":{\"request\":[],\"response\":\"string\"},\"joinPublicChat\":{\"request\":[{\"name\":\"id\",\"type\":\"int\"}],\"response\":\"int\"},\"sendMessage\":{\"request\":[{\"name\":\"id\",\"type\":\"int\"},{\"name\":\"message\",\"type\":\"string\"}],\"response\":\"int\"},\"exitPublicChat\":{\"request\":[{\"name\":\"id\",\"type\":\"int\"}],\"response\":\"int\"}}}");
  int registerClient(java.lang.CharSequence username, java.lang.CharSequence ipaddress, int port) throws org.apache.avro.AvroRemoteException;
  int exitClient(int id) throws org.apache.avro.AvroRemoteException;
  java.lang.CharSequence getListOfClients() throws org.apache.avro.AvroRemoteException;
  int joinPublicChat(int id) throws org.apache.avro.AvroRemoteException;
  int sendMessage(int id, java.lang.CharSequence message) throws org.apache.avro.AvroRemoteException;
  int exitPublicChat(int id) throws org.apache.avro.AvroRemoteException;

  @SuppressWarnings("all")
  public interface Callback extends AppServerInterface {
    public static final org.apache.avro.Protocol PROTOCOL = chat_app.AppServerInterface.PROTOCOL;
    void registerClient(java.lang.CharSequence username, java.lang.CharSequence ipaddress, int port, org.apache.avro.ipc.Callback<java.lang.Integer> callback) throws java.io.IOException;
    void exitClient(int id, org.apache.avro.ipc.Callback<java.lang.Integer> callback) throws java.io.IOException;
    void getListOfClients(org.apache.avro.ipc.Callback<java.lang.CharSequence> callback) throws java.io.IOException;
    void joinPublicChat(int id, org.apache.avro.ipc.Callback<java.lang.Integer> callback) throws java.io.IOException;
    void sendMessage(int id, java.lang.CharSequence message, org.apache.avro.ipc.Callback<java.lang.Integer> callback) throws java.io.IOException;
    void exitPublicChat(int id, org.apache.avro.ipc.Callback<java.lang.Integer> callback) throws java.io.IOException;
  }
}