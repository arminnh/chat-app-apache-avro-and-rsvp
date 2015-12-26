#include <click/config.h>
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/router.hh>
#include "rsvpelement.hh"
#include <stdexcept>
#include <ctime>

CLICK_DECLS

const void* RSVPObjectOfType(Packet* packet, uint8_t wanted_class_num) {
	uint8_t class_num;
	
	const RSVPObjectHeader* p = (const RSVPObjectHeader *) (packet->transport_header() ? packet->transport_header() : ((const unsigned char*) packet->data()) + sizeof(click_ip));
	
	p = (const RSVPObjectHeader*) (((const RSVPCommonHeader*) p) + 1);
	
	while (p < (const RSVPObjectHeader*) packet->end_data()) {
		readRSVPObjectHeader(p, &class_num, NULL);
		if (class_num == wanted_class_num) {
			return p;
		}
		
		p = nextRSVPObject(p);
	}
	
	return NULL;
}


void RSVPElement::push(int, Packet *packet) {
	if (_dead) {
		packet->kill();
		return;
	}

	// chop off IP header
	packet->pull(sizeof(click_ip));
	
	uint8_t msg_type;
	readRSVPCommonHeader((RSVPCommonHeader*) packet->data(), &msg_type, NULL, NULL);
	
	RSVPSession* session = (RSVPSession *) RSVPObjectOfType(packet, RSVP_CLASS_SESSION);
	RSVPNodeSession nodeSession = *session;
	RSVPFilterSpec* filterSpec;
	RSVPFlowspec* flowspec;
	
	RSVPPathState pathState;

	WritablePacket* reply;

	switch (msg_type) {
		case RSVP_MSG_PATH:
			clean();
			
			// if auto reservation is on, respond to the path message with a reservation message
			if (find(_pathStates, nodeSession) == _pathStates.end()) {
				click_chatter("%s: creating new path state for %s", _name.c_str(), IPAddress(nodeSession._dst_ip_address).unparse().c_str());

				if (_autoResv) {
					reply = replyToPathMessage(packet->clone());
				}
				
				packet->kill();
				
				output(0).push(reply);
			} else {
				// click_chatter("Updating path state for %s at %s", IPAddress(nodeSession._dst_ip_address).unparse().c_str(), _name.c_str());
			}

			updatePathState(packet->clone());

			break;
		case RSVP_MSG_RESV:
			if (find(_resvStates, nodeSession) == _resvStates.end()) {
				click_chatter("%s: creating new resv state for %s", _name.c_str(), IPAddress(nodeSession._dst_ip_address).unparse().c_str(), _name.c_str());
			} else {
				// click_chatter("%s: updating resv state for %s", _name.c_str(), IPAddress(nodeSession._dst_ip_address).unparse().c_str());
			}

			// get the necessary information in order to update the reservation
			filterSpec = (RSVPFilterSpec *) RSVPObjectOfType(packet, RSVP_CLASS_FILTER_SPEC);
			flowspec = (RSVPFlowspec *) RSVPObjectOfType(packet, RSVP_CLASS_FLOWSPEC);
			uint32_t refresh_period_r;
			readRSVPTimeValues((RSVPTimeValues *) RSVPObjectOfType(packet, RSVP_CLASS_TIME_VALUES), &refresh_period_r);
			
			updateReservation(*session, filterSpec, flowspec, refresh_period_r);
			break;
		case RSVP_MSG_PATHERR:
			// read rfc
			break;
		case RSVP_MSG_RESVERR:
			// read rfc
			break;
		case RSVP_MSG_PATHTEAR:
			// remove session, send resv tear
			break;
		case RSVP_MSG_RESVTEAR:
			// remove session, send path tear
			break;
		case RSVP_MSG_RESVCONF:
			// notify application if session present
			break;
		default:
			click_chatter("RSVPElement %s: no message type %d", _name.c_str(), msg_type);
	}
}

Packet* RSVPElement::pull(int){
	return NULL;
}

WritablePacket* RSVPElement::replyToPathMessage(Packet* pathMessage) {
	// copy session
	_session = * (RSVPSession *) RSVPObjectOfType(pathMessage, RSVP_CLASS_SESSION);
	// copy time values (for now)
	_timeValues = * (RSVPTimeValues *) RSVPObjectOfType(pathMessage, RSVP_CLASS_TIME_VALUES);

	// hop address will be the resv messages's destination
	RSVPHop* pathHop = (RSVPHop *) RSVPObjectOfType(pathMessage, RSVP_CLASS_RSVP_HOP);
	in_addr resvDestinationAddress; uint32_t lih;
	readRSVPHop(pathHop, &resvDestinationAddress, &lih);

	// resv's hop address is the host's own IP
	initRSVPHop(&_hop, _myIP, lih);

	RSVPSenderTemplate* senderTemplate;
	RSVPSenderTSpec* senderTSpec;
	if ((senderTemplate = (RSVPSenderTemplate *) RSVPObjectOfType(pathMessage, RSVP_CLASS_SENDER_TEMPLATE))
			&& (senderTSpec = (RSVPSenderTSpec *) RSVPObjectOfType(pathMessage, RSVP_CLASS_SENDER_TSPEC))) {
		_flowDescriptor = true;

		// reserve the capacity the sender provides
		_filterSpec = *senderTemplate;
		initRSVPObjectHeader(&_filterSpec.header, RSVP_CLASS_FILTER_SPEC, 1);
		initRSVPFlowspec(&_flowspec, senderTSpec);
	}

	WritablePacket* resvMessage = createResvMessage();

	addIPHeader(resvMessage, resvDestinationAddress, _myIP, _tos);

	uint32_t refresh_period;
	readRSVPTimeValues(&_timeValues, &refresh_period);

	RSVPResvState resvState;
	resvState.filterSpec = _filterSpec;
	resvState.flowspec = _flowspec;
	resvState.refresh_period_r = refresh_period;

	resvState.timer = new Timer(this);

	_reservations.set(_session, resvState);

	resvState.timer->initialize(this);
	resvState.timer->schedule_now();

	return resvMessage;
}

RSVPElement::RSVPElement()
{}

RSVPElement::~ RSVPElement()
{}

int RSVPElement::configure(Vector<String> &conf, ErrorHandler *errh) {
	_application = false;

	_autoResv = true;

	if (cp_va_kparse(conf, this, errh,
		"IP", cpkM + cpkP, cpIPAddress, &_myIP,
		"APPLICATION", 0, cpBool, &_application,
		"AUTORESV", 0, cpBool, &_autoResv, cpEnd) < 0) return -1;

	return 0;
}

int RSVPElement::initialize(ErrorHandler* errh) {
	// _timer.initialize(this);

	srand(time(NULL));

	_tos = 0;
	clean();

	return 0;
}

void RSVPElement::run_timer(Timer* timer) {
	clean();
	// figure out which session the timer belongs to
	const RSVPNodeSession* session;
	
	unsigned refresh_period;
	const RSVPPathState* pathState;
	const RSVPResvState* resvState;

	unsigned refresh_period_r;
// static int i = 0;
	if ((session = sessionForSenderTimer(timer))) {
		HashTable<RSVPNodeSession, RSVPPathState>::iterator it = find(_senders, *session);
		pathState = &it->second;
		sendPeriodicPathMessage(session, pathState);

		refresh_period_r = pathState->refresh_period_r;
	} else if ((session = (RSVPNodeSession *) sessionForReservationTimer(timer))) {
		HashTable<RSVPNodeSession, RSVPResvState>::iterator it = find(_reservations, *session);
		resvState = &it->second;
		// click_chatter("%s: sending periodic resv message %d", _name.c_str(), ++i);
		sendPeriodicResvMessage(session, resvState);

		refresh_period_r = resvState->refresh_period_r;
	} else {
		RSVPNode::run_timer(timer);
		return;
	}
	
	double randomized_refresh = ((double) rand() / RAND_MAX + 0.5) * refresh_period_r;
	// click_chatter("%s: setting randomized refresh to %f with R = %d", _name.c_str(), randomized_refresh, refresh_period_r);
	timer->reschedule_after_sec(randomized_refresh);

	return;
}

void RSVPElement::sendPeriodicPathMessage(const RSVPNodeSession* session, const RSVPPathState* pathState) {
	
	RSVPSession packetSession;
	initRSVPSession(&packetSession, session);
	RSVPHop hop;
	initRSVPHop(&hop, _myIP, 0);
	RSVPTimeValues timeValues;
	initRSVPTimeValues(&timeValues, pathState->refresh_period_r);

	WritablePacket* message = createPathMessage(&packetSession,
		&hop, &timeValues, &pathState->senderTemplate, &pathState->senderTSpec);
	addIPHeader(message, session->_dst_ip_address, _myIP, (uint8_t) _tos);
	output(0).push(message);
}

void RSVPElement::erasePathState(const RSVPNodeSession& session) {
	RSVPNode::erasePathState(session);
	_reservations.erase(find(_reservations, session));
}

void RSVPElement::sendPeriodicResvMessage(const RSVPNodeSession* session, const RSVPResvState* resvState) {
	HashTable<RSVPNodeSession, RSVPResvState>::const_iterator it = find(_reservations, *session);
	HashTable<RSVPNodeSession, RSVPPathState>::const_iterator pathit = find(_pathStates, *session);

	if (it == _reservations.end()) {
		click_chatter("%s: Trying to send resv message for nonexistent reservation.");
	}

	if (pathit == _pathStates.end()) {
		click_chatter("%s: Trying to send resv message for nonexitent path state.");
	}

	const RSVPPathState* pathState = &pathit->second;

	RSVPSession packetSession; initRSVPSession(&packetSession, session);
	RSVPHop hop; initRSVPHop(&hop, _myIP, 0);
	RSVPTimeValues timeValues; initRSVPTimeValues(&timeValues, resvState->refresh_period_r);

	WritablePacket* message = createResvMessage(&packetSession,
		&hop,
		&timeValues,
		NULL, // also implement & test resv conf
		&resvState->flowspec,
		&resvState->filterSpec);
	addIPHeader(message, pathState->previous_hop_node, _myIP, _tos);
	output(0).push(message);
}

const RSVPNodeSession* RSVPElement::sessionForSenderTimer(const Timer* timer) const {
	for (HashTable<RSVPNodeSession, RSVPPathState>::const_iterator it = _senders.begin(); it != _senders.end(); it++) {
		if (it->second.timer == timer) {
			return &it->first;
		}
	}

	return NULL;
}

const RSVPNodeSession* RSVPElement::sessionForReservationTimer(const Timer* timer) const {
	for (HashTable<RSVPNodeSession, RSVPResvState>::const_iterator it = _reservations.begin(); it != _reservations.end(); it++) {
		if (it->second.timer == timer) {
			return &it->first;
		}
	}

	return NULL;
}

int RSVPElement::sessionHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;

	int protocol_ID;
	bool police;
	int destination_port;
	
	in_addr destination_address;
	
	if (cp_va_kparse(conf, me, errh, 
		"DEST", cpkM, cpIPAddress, &destination_address, 
		"PROTOCOL", cpkM, cpUnsigned, &protocol_ID,
		"POLICE", cpkM, cpBool, &police,
		"PORT", cpkM, cpUnsigned, &destination_port, 
		cpEnd) < 0) return -1;
	
	initRSVPSession(&me->_session, destination_address, (uint8_t) protocol_ID, police, (uint16_t) destination_port);

	return 0;
}

int RSVPElement::hopHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;

	in_addr neighbor_address;
	int logical_interface_handle;

	if (cp_va_kparse(conf, me, errh, 
		"NEIGHBOR", cpkM, cpIPAddress, &neighbor_address, 
		"LIH", cpkM, cpUnsigned, &logical_interface_handle,  
		cpEnd) < 0) return -1;

	initRSVPHop(&me->_hop, neighbor_address, (uint32_t) logical_interface_handle);

	return 0;
}

int RSVPElement::errorSpecHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	
	in_addr error_node_address;
	bool inPlace;
	bool notGuilty;
	int errorCode;
	int errorValue;

	if (cp_va_kparse(conf, me, errh, 
		"ERROR_NODE_ADDRESS", cpkM, cpIPAddress, &error_node_address,
		"INPLACE", cpkM, cpBool, &inPlace,
		"NOTGUILTY", cpkM, cpBool, &notGuilty,
		"ERROR_CODE", cpkM, cpUnsigned, &errorCode,
		"ERROR_VALUE", cpkM, cpUnsigned, &errorValue,
		cpEnd) < 0) return -1;
	
	initRSVPErrorSpec(&me->_errorSpec, error_node_address, inPlace, notGuilty, (uint8_t) errorCode, (uint16_t) errorValue);

	return 0;
}

int RSVPElement::timeValuesHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;

	int refresh_period_r;

	if (cp_va_kparse(conf, me, errh, 
		"REFRESH", cpkM, cpUnsigned, &refresh_period_r, 
		cpEnd) < 0) return -1;
	
	initRSVPTimeValues(&me->_timeValues, (uint32_t) refresh_period_r);

	return 0;
}

int RSVPElement::resvConfObjectHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;

	in_addr receiver_address;

	if (cp_va_kparse(conf, me, errh,
		"RECEIVER_ADDRESS", cpkM, cpIPAddress, &receiver_address,
		cpEnd) < 0) return -1;

	initRSVPResvConf(&me->_resvConf, receiver_address);

	me->_resvConf_given = true;

	return 0;
}

int RSVPElement::pathHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");
	bool refresh = true;
	
	readRSVPSession(&me->_session, &destinationIP, NULL, NULL, NULL);

	//click_chatter("pathHandle: destinationIP: %s", IPAddress(destinationIP).unparse().c_str());
	if (cp_va_kparse(conf, me, errh,
		"TTL", 0, cpInteger, &me->_TTL,
		"REFRESH", 0, cpBool, &refresh, cpEnd) < 0) return -1;
	
	uint32_t r;
	
	RSVPNodeSession nodeSession(me->_session);
	RSVPPathState pathState;
	pathState.senderTemplate = me->_senderTemplate;
	pathState.senderTSpec = me->_senderTSpec;
	readRSVPTimeValues(&me->_timeValues, &pathState.refresh_period_r);
	pathState.timer = new Timer(me);
	pathState.timer->initialize(me);
	
	me->_senders.set(nodeSession, pathState);
	
	if (refresh) {
		pathState.timer->schedule_now();
	} else {
		me->sendPeriodicPathMessage(&nodeSession, &pathState);
		me->_senders.erase(find(me->_senders, nodeSession));
	}
	
	//Element* classifier = me->router()->find("in_cl", me);
	//click_chatter("find returned %p", (void*) classifier);
	
	me->clean();
	return 0;
}

int RSVPElement::resvHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");

	if (cp_va_kparse(conf, me, errh,
		"DST", cpkP, cpIPAddress, &destinationIP,
		"TTL", 0, cpInteger, &me->_TTL, cpEnd) < 0) return -1;
	
	WritablePacket* message = me->createResvMessage();
	me->addIPHeader(message, destinationIP, me->_myIP, (uint8_t) me->_tos);
	me->output(0).push(message);
	
	me->clean();
	return 0;
}

int RSVPElement::pathErrHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");

	if (cp_va_kparse(conf, me, errh,
		"DST", cpkP, cpIPAddress, &destinationIP,
		"TTL", 0, cpInteger, &me->_TTL, cpEnd) < 0) return -1;
	
	WritablePacket* message = me->createPathErrMessage();
	me->addIPHeader(message, destinationIP, me->_myIP, (uint8_t) me->_tos);
	me->output(0).push(message);
	
	me->clean();

	return 0;
}

int RSVPElement::resvErrHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");

	if (cp_va_kparse(conf, me, errh,
		"DST", cpkP, cpIPAddress, &destinationIP,
		"TTL", 0, cpInteger, &me->_TTL, cpEnd) < 0) return -1;
	
	WritablePacket* message = me->createResvErrMessage();
	me->addIPHeader(message, destinationIP, me->_myIP, (uint8_t) me->_tos);
	me->output(0).push(message);
	
	me->clean();

	return 0;
}

int RSVPElement::pathTearHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");

	if (cp_va_kparse(conf, me, errh,
		"DST", cpkP, cpIPAddress, &destinationIP,
		"TTL", 0, cpInteger, &me->_TTL, cpEnd) < 0) return -1;
	
	WritablePacket* message = me->createPathTearMessage();
	me->addIPHeader(message, destinationIP, me->_myIP, (uint8_t) me->_tos);
	me->output(0).push(message);
	
	me->clean();

	return 0;
}

int RSVPElement::resvTearHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");

	if (cp_va_kparse(conf, me, errh,
		"DST", cpkP, cpIPAddress, &destinationIP,
		"TTL", 0, cpInteger, &me->_TTL, cpEnd) < 0) return -1;
	
	WritablePacket* message = me->createResvTearMessage();
	me->addIPHeader(message, destinationIP, me->_myIP, (uint8_t) me->_tos);
	me->output(0).push(message);
	
	me->clean();

	return 0;
}

int RSVPElement::resvConfHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement * me = (RSVPElement *) e;
	in_addr destinationIP = IPAddress("0.0.0.0");

	if (cp_va_kparse(conf, me, errh,
		"DST", cpkP, cpIPAddress, &destinationIP,
		"TTL", 0, cpInteger, &me->_TTL, cpEnd) < 0) return -1;
	
	WritablePacket* message = me->createResvConfMessage();
	me->addIPHeader(message, destinationIP, me->_myIP, (uint8_t) me->_tos);
	me->output(0).push(message);
	
	me->clean();
	return 0;
}

int RSVPElement::tosHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh) {
	RSVPElement* me = (RSVPElement *) e;
	
	cp_va_kparse(conf, e, errh, "TOS", cpkP + cpkM, cpInteger, &me->_tos, cpEnd);

	return 0;
}

int RSVPElement::scopeHandle(const String &conf, Element *e, void * thunk, ErrorHandler *errh)
{
	RSVPElement *me = (RSVPElement *) e;

	in_addr src_address;

	if (cp_va_kparse(conf, me, errh, "SRC_ADDRESS", cpkM, cpIPAddress, &src_address, cpEnd) < 0) return -1;

	me->_scope_src_addresses.push_back(src_address);

	return 0;
}

int RSVPElement::senderDescriptorHandle(const String &conf, Element *e, void *thunk, ErrorHandler *errh)
{
	RSVPElement *me = (RSVPElement *) e;

	double tbr, tbs, pdr;

	in_addr senderTemplate_src_address;
	unsigned senderTemplate_src_port;

	float senderTSpec_token_bucket_rate;
	float senderTSpec_token_bucket_size;
	float senderTSpec_peak_data_rate;
	int senderTSpec_minimum_policed_unit;
	int senderTSpec_maximum_packet_size;

	if (!cp_va_kparse(conf, me, errh,
		// sender template
		"SRC_ADDRESS", cpkM, cpIPAddress, &senderTemplate_src_address,
		"SRC_PORT", cpkM, cpUnsigned, &senderTemplate_src_port,
		// sender tspec
		"TOKEN_BUCKET_RATE", cpkM, cpDouble, &tbr,
		"TOKEN_BUCKET_SIZE", cpkM, cpDouble, &tbs,
		"PEAK_DATA_RATE", cpkM, cpDouble, &pdr,
		"MINIMUM_POLICED_UNIT", cpkM, cpInteger, &senderTSpec_minimum_policed_unit,
		"MAXIMUM_PACKET_SIZE", cpkM, cpInteger, &senderTSpec_maximum_packet_size,
		cpEnd)) return -1;

	senderTSpec_token_bucket_rate = tbr;
	senderTSpec_token_bucket_size = tbs;
	senderTSpec_peak_data_rate = pdr;

	initRSVPSenderTemplate(&me->_senderTemplate, senderTemplate_src_address, (uint16_t) senderTemplate_src_port);
	initRSVPSenderTSpec(&me->_senderTSpec, senderTSpec_token_bucket_rate,
		senderTSpec_token_bucket_size, senderTSpec_peak_data_rate,
		(uint32_t) senderTSpec_minimum_policed_unit, (uint32_t) senderTSpec_maximum_packet_size);

	me->_senderDescriptor = true;

	return 0;
}

int RSVPElement::flowDescriptorHandle(const String& conf, Element *e, void *thunk, ErrorHandler *errh)
{
	RSVPElement* me = (RSVPElement *) e;

	double tbr, tbs, pdr;

	in_addr filterSpec_src_address;
	unsigned filterSpec_src_port;

	float flowspec_token_bucket_rate;
	float flowspec_token_bucket_size;
	float flowspec_peak_data_rate;
	int flowspec_minimum_policed_unit;
	int flowspec_maximum_packet_size;

	if (!cp_va_kparse(conf, me, errh,
		// filter spec
		"SRC_ADDRESS", cpkM, cpIPAddress, &filterSpec_src_address,
		"SRC_PORT", cpkM, cpUnsigned, &filterSpec_src_port,
		// flowspec
		"TOKEN_BUCKET_RATE", cpkM, cpDouble, &tbr,
		"TOKEN_BUCKET_SIZE", cpkM, cpDouble, &tbs,
		"PEAK_DATA_RATE", cpkM, cpDouble, &pdr,
		"MINIMUM_POLICED_UNIT", cpkM, cpInteger, &flowspec_minimum_policed_unit,
		"MAXIMUM_PACKET_SIZE", cpkM, cpInteger, &flowspec_maximum_packet_size,
		cpEnd)) return -1;

	flowspec_token_bucket_rate = tbr;
	flowspec_token_bucket_size = tbs;
	flowspec_peak_data_rate = pdr;

	initRSVPFilterSpec(&me->_filterSpec, filterSpec_src_address, (uint16_t) filterSpec_src_port);
	initRSVPFlowspec(&me->_flowspec, flowspec_token_bucket_rate,
		flowspec_token_bucket_size, flowspec_peak_data_rate,
		(uint32_t) flowspec_minimum_policed_unit, (uint32_t) flowspec_maximum_packet_size);

	me->_flowDescriptor = true;

	return 0;
}

String RSVPElement::getTTLHandle(Element *e, void * thunk) {
	RSVPElement *me = (RSVPElement *) e;
	return String((int) me->_TTL);
}

void RSVPElement::add_handlers() {
	RSVPNode::add_handlers();

	// types of messages
	add_write_handler("path", &pathHandle, (void *) 0);
	add_write_handler("resv", &resvHandle, (void *) 0);
	add_write_handler("patherr", &pathErrHandle, (void *) 0);
	add_write_handler("resverr", &resvErrHandle, (void *) 0);
	add_write_handler("pathtear", &pathTearHandle, (void *) 0);
	add_write_handler("resvtear", &resvTearHandle, (void *) 0);
	add_write_handler("resvconf", &resvConfHandle, (void *) 0);
	add_write_handler("tos", &tosHandle, (void *) 0);
	
	// types of objects
	add_write_handler("session", &sessionHandle, (void *) 0);
	add_write_handler("hop", &hopHandle, (void *)0);
	add_write_handler("errorspec", &errorSpecHandle, (void *) 0);
	add_write_handler("timevalues", &timeValuesHandle, (void *) 0);
	add_write_handler("resvconfobj", &resvConfObjectHandle, (void *) 0);
	add_write_handler("scope", &scopeHandle, (void *) 0);
	add_write_handler("senderdescriptor", &senderDescriptorHandle, (void *) 0);
	add_write_handler("flowdescriptor", &flowDescriptorHandle, (void *) 0);

	// random read handler
	add_read_handler("TTL", &getTTLHandle, (void *) 0);
}

WritablePacket* RSVPElement::createPacket(uint16_t packetSize) const
{
	unsigned headroom = sizeof(click_ip) + sizeof(click_ether);
	unsigned tailroom = 0;

	WritablePacket* message = Packet::make(headroom, 0, packetSize, tailroom);

	if (!message) click_chatter("RSVPElement::createPathMessage: cannot make element!");

	memset(message->data(), 0, message->length());

	return message;
}

WritablePacket* RSVPElement::createPathMessage(const RSVPSession* p_session,
	const RSVPHop* p_hop,
	const RSVPTimeValues* p_timeValues,
	const RSVPSenderTemplate* p_senderTemplate,
	const RSVPSenderTSpec* p_senderTSpec) const
{
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPHop) +
		sizeof(RSVPTimeValues) +
		(p_senderTemplate && p_senderTSpec ?
			sizeof(RSVPSenderTemplate) +
			sizeof(RSVPSenderTSpec)
			: 0);
	
	WritablePacket* message = createPacket(packetSize);
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPHop* hop                   = (RSVPHop *)          (session      + 1);
	RSVPTimeValues* timeValues     = (RSVPTimeValues *)   (hop          + 1);
	RSVPSenderTemplate* senderTemplate = (RSVPSenderTemplate *) (timeValues + 1);
	RSVPSenderTSpec* senderTSpec   = (RSVPSenderTSpec *)  (senderTemplate + 1);

	initRSVPCommonHeader(commonHeader, RSVP_MSG_PATH, _TTL, packetSize);
	*session = *p_session;
	*hop = *p_hop;
	*timeValues = *p_timeValues;
	if (p_senderTemplate && p_senderTSpec) {
		*senderTemplate = *p_senderTemplate;
		*senderTSpec = *p_senderTSpec;
	}
	
	
	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

WritablePacket* RSVPElement::createResvMessage() const {
	return createResvMessage(&_session, &_hop, &_timeValues, _resvConf_given ? &_resvConf : NULL,
		_flowDescriptor ? &_flowspec : NULL,
		_flowDescriptor ? &_filterSpec : NULL);
}

WritablePacket* RSVPElement::createResvMessage(const RSVPSession* p_session,
		const RSVPHop* p_hop,
		const RSVPTimeValues* p_timeValues,
		const RSVPResvConf* p_resvConf,
		const RSVPFlowspec* p_flowspec,
		const RSVPFilterSpec* p_filterSpec) const {
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPHop) +
		sizeof(RSVPTimeValues) +
		(p_resvConf ? sizeof(RSVPResvConf) : 0) +
		sizeofRSVPScopeObject(_scope_src_addresses.size()) +
		sizeof(RSVPStyle) +
		((p_flowspec && p_filterSpec) ? sizeof(RSVPFlowspec) + sizeof(RSVPFilterSpec) : 0);

	WritablePacket* message = createPacket(packetSize);
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPHop* hop                   = (RSVPHop *)          (session      + 1);
	RSVPTimeValues* timeValues     = (RSVPTimeValues *)   (hop          + 1);
	RSVPResvConf* resvConf         = (RSVPResvConf *)     (timeValues   + 1);
	RSVPStyle* style               = (RSVPStyle *)        initRSVPScope((RSVPObjectHeader *) (resvConf + (p_resvConf ? 1 : 0)), _scope_src_addresses);
	RSVPFlowspec* flowspec         = (RSVPFlowspec *)     (style        + 1);
	RSVPFilterSpec* filterSpec     = (RSVPFilterSpec *)   (flowspec     + 1);
	
	initRSVPCommonHeader(commonHeader, RSVP_MSG_RESV, _TTL, packetSize);
	*session = *p_session;
	*hop = *p_hop;
	*timeValues = *p_timeValues;
	initRSVPStyle(style);
	
	if (p_resvConf) {
		*resvConf = *p_resvConf;
	}
	
	if (p_flowspec && p_filterSpec) {
		*flowspec = *p_flowspec;
		*filterSpec = *p_filterSpec;
	}

	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

WritablePacket* RSVPElement::createPathErrMessage() const
{
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPErrorSpec) +
		sizeofRSVPScopeObject(_scope_src_addresses.size()) +
		sizeof(RSVPStyle) +
		(_senderDescriptor ? sizeof(RSVPSenderTemplate) + sizeof(RSVPSenderTSpec) : 0);
	unsigned tailroom = 0;
	
	WritablePacket* message = createPacket(packetSize);
	
	memset(message->data(), 0, message->length());
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPErrorSpec* errorSpec       = (RSVPErrorSpec *)    (session      + 1);
	RSVPSenderTemplate* senderTemplate = (RSVPSenderTemplate *) (errorSpec + 1);
	RSVPSenderTSpec* senderTSpec   = (RSVPSenderTSpec *)  initRSVPScope((RSVPObjectHeader *) (senderTemplate + 1), _scope_src_addresses);
	
	initRSVPCommonHeader(commonHeader, RSVP_MSG_PATHERR, _TTL, packetSize);
	*session = _session;
	*errorSpec = _errorSpec;
	
	if (_senderDescriptor) {
		*senderTSpec = _senderTSpec;
		*senderTemplate = _senderTemplate;
	}

	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

WritablePacket* RSVPElement::createResvErrMessage() const
{
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPHop) +
		sizeof(RSVPErrorSpec) +
		sizeofRSVPScopeObject(_scope_src_addresses.size()) +
		sizeof(RSVPStyle) +
		(_flowDescriptor ? sizeof(RSVPFilterSpec) + sizeof(RSVPFlowspec) : 0);
	
	WritablePacket* message = createPacket(packetSize);
	
	memset(message->data(), 0, message->length());
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPHop* hop                   = (RSVPHop *)          (session      + 1);
	RSVPErrorSpec* errorSpec       = (RSVPErrorSpec *)    (hop          + 1);
	RSVPStyle* style               = (RSVPStyle *)        initRSVPScope((RSVPObjectHeader *) (errorSpec + 1), _scope_src_addresses);
	RSVPFilterSpec* filterSpec     = (RSVPFilterSpec *)   (style        + 1);
	RSVPFlowspec* flowspec         = (RSVPFlowspec *)     (filterSpec   + 1);
	
	initRSVPCommonHeader(commonHeader, RSVP_MSG_RESVERR, _TTL, packetSize);
	*session = _session;
	*hop = _hop;
	*errorSpec = _errorSpec;
	initRSVPStyle(style);
	
	if (_flowDescriptor) {
		*filterSpec = _filterSpec;
		*flowspec = _flowspec;
	}

	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

WritablePacket* RSVPElement::createPathTearMessage() const
{
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPHop) +
		(_senderDescriptor ? sizeof(RSVPSenderTemplate) + sizeof(RSVPSenderTSpec) : 0);
	
	WritablePacket* message = createPacket(packetSize);
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPHop* hop                   = (RSVPHop *)          (session      + 1);
	RSVPSenderTemplate* senderTemplate = (RSVPSenderTemplate *) (hop    + 1);
	RSVPSenderTSpec* senderTSpec   = (RSVPSenderTSpec *)  (senderTemplate + 1);
	
	initRSVPCommonHeader(commonHeader, RSVP_MSG_PATHTEAR, _TTL, packetSize);
	*session = _session;
	*hop = _hop;
	
	if (_senderDescriptor) {
		*senderTemplate = _senderTemplate;
		*senderTSpec = _senderTSpec;
	}

	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

WritablePacket* RSVPElement::createResvTearMessage() const
{
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPHop) +
		sizeofRSVPScopeObject(_scope_src_addresses.size()) +
		sizeof(RSVPStyle) +
		(_flowDescriptor ? sizeof(RSVPFilterSpec) + sizeof(RSVPFlowspec) : 0);
	
	WritablePacket* message = createPacket(packetSize);
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPHop* hop                   = (RSVPHop *)          (session      + 1);
	RSVPStyle* style               = (RSVPStyle *)        initRSVPScope((RSVPObjectHeader *) (hop + 1), _scope_src_addresses);
	RSVPFilterSpec* filterSpec     = (RSVPFilterSpec *)   (style        + 1);
	RSVPFlowspec* flowspec         = (RSVPFlowspec *)     (filterSpec   + 1);
	
	initRSVPCommonHeader(commonHeader, RSVP_MSG_RESVTEAR, _TTL, packetSize);
	*session = _session;
	*hop = _hop;
	initRSVPStyle(style);
	
	if (_flowDescriptor) {
		*filterSpec = _filterSpec;
		*flowspec = _flowspec;
	}

	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

WritablePacket* RSVPElement::createResvConfMessage() const
{
	uint16_t packetSize =
		sizeof(RSVPCommonHeader) +
		sizeof(RSVPSession) +
		sizeof(RSVPErrorSpec) +
		sizeof(RSVPResvConf) +
		sizeof(RSVPStyle) +
		(_flowDescriptor ? sizeof(RSVPFilterSpec) + sizeof(RSVPFlowspec) : 0);
	
	WritablePacket* message = createPacket(packetSize);
	
	RSVPCommonHeader* commonHeader = (RSVPCommonHeader *) (message->data());
	RSVPSession* session           = (RSVPSession *)      (commonHeader + 1);
	RSVPErrorSpec* errorSpec       = (RSVPErrorSpec *)    (session      + 1);
	RSVPResvConf* resvConf         = (RSVPResvConf *)     (errorSpec    + 1);
	RSVPStyle* style               = (RSVPStyle *)        (resvConf     + 1);
	RSVPFilterSpec* filterSpec     = (RSVPFilterSpec *)   (style        + 1);
	RSVPFlowspec* flowspec         = (RSVPFlowspec *)     (filterSpec   + 1);
	
	initRSVPCommonHeader(commonHeader, RSVP_MSG_RESVCONF, _TTL, packetSize);
	*session = _session;
	*errorSpec = _errorSpec;
	*resvConf = _resvConf;
	initRSVPStyle(style);
	
	if (_flowDescriptor) {
		*filterSpec = _filterSpec;
		*flowspec = _flowspec;
	}

	commonHeader->RSVP_checksum = click_in_cksum((unsigned char *) commonHeader, packetSize);
	
	return message;
}

void RSVPElement::die() {
	RSVPNode::die();
	
	Timer* timer;
	for (HashTable<RSVPNodeSession, RSVPPathState>::iterator it = _senders.begin(); it != _senders.end(); it++) {
		timer = it->second.timer;
		if (timer) {
			timer->unschedule();
		}
	}
	for (HashTable<RSVPNodeSession, RSVPResvState>::iterator it = _reservations.begin(); it != _reservations.end(); it++) {
		timer = it->second.timer;
		if (timer) {
			timer->unschedule();
		}
	}
}

void RSVPElement::clean() {
	_TTL = 250;
	
	memset(&_session, 0, sizeof(RSVPSession));
	memset(&_errorSpec, 0, sizeof(RSVPErrorSpec));
	initRSVPHop(&_hop, _myIP, sizeof(RSVPHop));
	initRSVPTimeValues(&_timeValues, 5);
	
	_senderDescriptor = false;
	memset(&_senderTemplate, 0, sizeof(RSVPSenderTemplate));
	memset(&_senderTSpec, 0, sizeof(RSVPSenderTSpec));

	_flowDescriptor = false;
	memset(&_flowspec, 0, sizeof(RSVPFlowspec));
	memset(&_filterSpec, 0, sizeof(RSVPFilterSpec));

	_resvConf_given = false;
	memset(&_resvConf, 0, sizeof(RSVPResvConf));

	_scope_src_addresses.clear();
}	

CLICK_ENDDECLS
EXPORT_ELEMENT(RSVPElement)
