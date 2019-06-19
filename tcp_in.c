#include "tcp.h"
#include "tcp_sock.h"
#include "tcp_timer.h"

#include "log.h"
#include "ring_buffer.h"
#include "tcp_packet_cache.h"
#include <stdlib.h>

// handling incoming packet for TCP_LISTEN state
//
// 1. malloc a child tcp sock to serve this connection request;
// 2. send TCP_SYN | TCP_ACK by child tcp sock;
// 3. hash the child tcp sock into established_table (because the 4-tuple
//    is determined).

void tcp_state_listen(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	log(DEBUG, "in tcp_state_listen function");

	struct tcp_sock* c_tsk;

	c_tsk = alloc_tcp_sock();
	c_tsk->sk_sip = cb->daddr;
	c_tsk->sk_dip = cb->saddr;
	c_tsk->sk_sport = cb->dport;
	c_tsk->sk_dport = cb->sport;

	c_tsk->rcv_nxt = cb->seq_end;

	c_tsk->parent = tsk;
	c_tsk->snd_wnd = cb->rwnd;


	list_add_tail(&c_tsk->list, &tsk->listen_queue);

	tcp_send_control_packet(c_tsk, TCP_SYN | TCP_ACK);

	tcp_set_state(c_tsk, TCP_SYN_RECV);

	if (tcp_hash(c_tsk)) {
		log(ERROR, "insert into established_table failed.");
		return;
	}
}

// handling incoming packet for TCP_CLOSED state, by replying TCP_RST
void tcp_state_closed(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	tcp_send_reset(cb);
}

// handling incoming packet for TCP_SYN_SENT state
//
// If everything goes well (the incoming packet is TCP_SYN|TCP_ACK), reply with
// TCP_ACK, and enter TCP_ESTABLISHED state, notify tcp_sock_connect; otherwise,
// reply with TCP_RST.
void tcp_state_syn_sent(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	// fprintf(stdout, "TODO:[tcp_in.c][tcp_state_syn_sent] implement this function please.\n");
	if (!(cb->flags &(TCP_SYN | TCP_ACK)) || cb->ack!=tsk->snd_nxt)
	{
		tcp_send_reset(cb);
		return;
	}
	tsk->rcv_nxt = cb->seq_end;
	tsk->snd_wnd = cb->rwnd;
    tsk->snd_una = cb->ack;
	tcp_send_control_packet(tsk, TCP_ACK);
	tcp_set_state(tsk, TCP_ESTABLISHED);
	wake_up(tsk->wait_connect);
}

// update the snd_wnd of tcp_sock
//
// if the snd_wnd before updating is zero, notify tcp_sock_send (wait_send)
static inline void tcp_update_window(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u16 old_snd_wnd = tsk->snd_wnd;
	tsk->snd_wnd = min(cb->rwnd,4000);
	printf("update snd_wnd:%d->%d\n",old_snd_wnd,cb->rwnd);
	if (old_snd_wnd == 0)
		wake_up(tsk->wait_send);
}

// update the snd_wnd safely: cb->ack should be between snd_una and snd_nxt
static inline void tcp_update_window_safe(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	if (less_or_equal_32b(tsk->snd_una, cb->ack) && less_or_equal_32b(cb->ack, tsk->snd_nxt))
		tcp_update_window(tsk, cb);
}

// handling incoming ack packet for tcp sock in TCP_SYN_RECV state
//
// 1. remove itself from parent's listen queue;
// 2. add itself to parent's accept queue;
// 3. wake up parent (wait_accept) since there is established connection in the
//    queue.
void tcp_state_syn_recv(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	// fprintf(stdout, "TODO:[tcp_in.c][tcp_state_syn_recv] implement this function please.\n");
	list_delete_entry(&tsk->list);
	tcp_sock_accept_enqueue(tsk);

    tsk->rcv_nxt = cb->seq_end;
	tsk->snd_wnd = cb->rwnd;
    //tsk->snd_una = cb->ack;

	tcp_set_state(tsk, TCP_ESTABLISHED);
	wake_up(tsk->parent->wait_accept);
}

#ifndef max
#	define max(x,y) ((x)>(y) ? (x) : (y))
#endif

// check whether the sequence number of the incoming packet is in the receiving
// window
static inline int is_tcp_seq_valid(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u32 rcv_end = tsk->rcv_nxt + max(tsk->rcv_wnd, 1);
	if (less_than_32b(cb->seq, rcv_end) && less_or_equal_32b(tsk->rcv_nxt, cb->seq_end)) {
		return 1;
	}
	else {
		log(ERROR, "received packet with invalid seq, drop it.");
		return 0;
	}
}

// put the payload of the incoming packet into rcv_buf, and notify the
// tcp_sock_read (wait_recv)
int tcp_recv_data(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	// fprintf(stdout, "TODO:[tcp_in.c][tcp_recv_data] implement this function please.\n");
	write_ring_buffer(tsk->rcv_buf, cb->payload, cb->pl_len);
	tsk->rcv_wnd -=cb->pl_len;
	wake_up(tsk->wait_recv);
	return 0;
}
void tcp_ack_data(struct tcp_sock * tsk,struct tcp_cb * cb)
{
    u32 ack = cb->ack;
    char new_acked = 0;
    if(ack<=tsk->snd_nxt && ack >= tsk->snd_una)
    {
        struct tcp_packet_cache * packet_cache_item,*q;
        list_for_each_entry_safe(packet_cache_item,q,&tsk->send_buf,list)
        {
            if(packet_cache_item->seq_end <= ack)
            // acked
            {
                if(packet_cache_item->seq_end > tsk->snd_una)
                {
                    tsk->snd_una = packet_cache_item->seq_end;
                    new_acked = 1;
                }

                list_delete_entry(&packet_cache_item->list);

                printf("acked:[%d,%d)\n",packet_cache_item->seq,packet_cache_item->seq_end);
                //free(packet_cache_item);
            }
        }
        if(new_acked)
        {
            wake_up(tsk->wait_send);
        }
        if(!list_empty(&tsk->send_buf))
        {
            tsk->retrans_timer.timeout = tsk->RTO;
            tsk->retrans_timer.enable = 1;
        }
        else
        {
            tcp_unset_retrans_timer(tsk);//unset retransfer timer.
        }
    }
}
// Process an incoming packet as follows:
// 	 1. if the state is TCP_CLOSED, hand the packet over to tcp_state_closed;
// 	 2. if the state is TCP_LISTEN, hand it over to tcp_state_listen;
// 	 3. if the state is TCP_SYN_SENT, hand it to tcp_state_syn_sent;
// 	 4. check whether the sequence number of the packet is valid, if not, drop
// 	    it;
// 	 5. if the TCP_RST bit of the packet is set, close this connection, and
// 	    release the resources of this tcp sock;
// 	 6. if the TCP_SYN bit is set, reply with TCP_RST and close this connection,
// 	    as valid TCP_SYN has been processed in step 2 & 3;
// 	 7. check if the TCP_ACK bit is set, since every packet (except the first
//      SYN) should set this bit;
//   8. process the ack of the packet: if it ACKs the outgoing SYN packet,
//      establish the connection; if it ACKs new data, update the window;
//      if it ACKs the outgoing FIN packet, switch to corresponding state;
//   9. process the payload of the packet: call tcp_recv_data to receive data;
//  10. if the TCP_FIN bit is set, update the TCP_STATE accordingly;
//  11. at last, do not forget to reply with TCP_ACK if the connection is alive.
// Process the incoming packet according to TCP state machine.
void tcp_process(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	//fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	char cb_flags[32];
	tcp_copy_flags_to_str(cb->flags, cb_flags);
    if (cb->flags !=TCP_ACK)
        log(DEBUG, "recived tcp packet %s", cb_flags);
    if(tsk->state == TCP_CLOSED)
    {
        //tcp_state_closed(tsk,cb,packet);
        return ;
    }
    if((tsk->state == TCP_LISTEN) && (cb->flags & TCP_SYN ))
    {
        tcp_state_listen(tsk,cb,packet);
        return ;
    }
    if((tsk->state == TCP_SYN_SENT) && (cb->flags & (TCP_SYN|TCP_ACK)))
    {
        tcp_state_syn_sent(tsk, cb, packet);
        tcp_ack_data(tsk,cb);
		return;
    }

	if ((tsk->state == TCP_SYN_RECV) && (cb->flags &TCP_ACK))
	{
		tcp_state_syn_recv(tsk, cb, packet);
		tcp_ack_data(tsk,cb);
		return;
	}

    if (cb->flags & TCP_RST)
	{
		//close this connection, and release the resources of this tcp sock
		tcp_set_state(tsk, TCP_CLOSED);
		tcp_unhash(tsk);
		return;
	}

	if (cb->flags & TCP_SYN)
	{
		//reply with TCP_RST and close this connection
		tcp_send_reset(cb);
		tcp_set_state(tsk, TCP_CLOSED);
		tcp_unhash(tsk);
		return;
	}

    if (!(cb->flags & TCP_ACK))
	{
		//drop
		log(ERROR, "received tcp packet without ack, drop it.");
		return;
	}



	if ((cb->flags & TCP_FIN) && tsk->state==TCP_ESTABLISHED && cb->seq == tsk->rcv_nxt)
	{
		//update the TCP_STATE accordingly
        //passive close.

		tcp_ack_data(tsk,cb);
		tsk->rcv_nxt = cb->seq_end;
		tcp_send_control_packet(tsk, TCP_ACK);
		tcp_set_state(tsk, TCP_CLOSE_WAIT);
        printf("[TCP_ESTABLISH]: passive close,send ACK(ack=%d,seq=%d),change to TCP_CLOSE_WAIT\n",tsk->rcv_nxt,tsk->snd_nxt);

        //wake up ,to driver process to call tcp_close()
        wake_up(tsk->wait_recv);
		return;
	}
	if (tsk->state == TCP_FIN_WAIT_1 && (cb->flags & TCP_ACK) && cb->ack == tsk->snd_nxt)
	{
        //active close:current in TCP_FIN_WAIT_1,
        //when recv TCP_ACK,THEN go to  TCP_FIN_WAIT_2
        tcp_ack_data(tsk,cb);
		tcp_set_state(tsk, TCP_FIN_WAIT_2);
		return;
	}

	if (tsk->state == TCP_FIN_WAIT_2 && (cb->flags & TCP_FIN) && cb->seq == tsk->rcv_nxt)
	{
        //active close : current in TCP_FIN_WAIT_2
        //when recv TCP_FIN ,Then go to TIME_WAIT

		//tsk->rcv_nxt = cb->seq_end;

		tcp_ack_data(tsk,cb);
        tsk->rcv_nxt = cb->seq_end;
		tcp_send_control_packet(tsk, TCP_ACK);

		// start a timer
		tcp_set_timewait_timer(tsk);

		tcp_set_state(tsk, TCP_TIME_WAIT);
		return;
	}

	if (tsk->state == TCP_LAST_ACK && (cb->flags & TCP_ACK) && cb->ack == tsk->snd_nxt )
	{
        //passive close:current in LAST_ACK
        //when recv ACK,Then go to TCP_CLOSED
        tcp_ack_data(tsk,cb);
		tcp_set_state(tsk, TCP_CLOSED);
		tcp_unhash(tsk);
		return;
	}

    //process the ack of the packet
	//update rcv_wnd
	//update advised wnd

	if (!is_tcp_seq_valid(tsk, cb))
	{
		// drop
		log(ERROR, "received tcp packet with invalid seq, drop it.");
		tcp_send_control_packet(tsk, TCP_ACK);
		return;
	}
    tcp_ack_data(tsk,cb);

    if (cb->ack < tsk->snd_una)
        //receive very early reply ack
    {
        tcp_send_control_packet(tsk,TCP_ACK);
    }
    if(cb->ack > tsk->snd_nxt)
        //out of range,ack
    {
        printf("receive ask(%d) > snd_nxt(%d),drop it!\n",cb->ack,tsk->snd_nxt);
        return ;
    }

	//recive data,it has tcp_payload
	if (cb->pl_len > 0)
	{
	    u32 seq = cb->seq;
        //ack this packet.
        u32 seq_end =cb->seq_end;
        //first insert this payload to ofo

        struct tcp_payload_cache * new_item = (struct tcp_payload_cache*) malloc(sizeof(struct tcp_payload_cache));
        struct tcp_payload_cache * q;
        new_item->seq = seq;
        new_item->seq_end = seq_end;
        new_item->payload = (char *)malloc(cb->pl_len);
        new_item->len = cb->pl_len;

        memcpy(new_item->payload,cb->payload,cb->pl_len);

        struct tcp_payload_cache * item;
        struct list_head * insert_pos=NULL;
        if(!list_empty(&tsk->rcv_ofo_buf))
        {
            list_for_each_entry_safe(item,q,&tsk->rcv_ofo_buf,list)
            {
                if(item->seq==new_item->seq && item->seq_end==new_item->seq_end)
                //duplicate payload
                {
                    //list_delete_entry(&item->list);
                    //free(item);
                    break;
                }
                else if(new_item->seq_end <= item->seq )
                {
                    //list_insert(&new_item->list,item->list.prev,&item->list);
                    insert_pos = item->list.prev;
                    break;
                }
                else if (item->seq_end <= new_item->seq  )
                {
                    if(item->list.next == &tsk->rcv_ofo_buf)
                    //add to the tail
                    {
                        insert_pos = &item->list;
                        break;
                    }
                    else
                    {
                        continue;
                    }
                }
            }
            if(insert_pos != NULL)
            {
                list_insert(&new_item->list,insert_pos,insert_pos->next);
            }
            else
                //it cannot be add the recv ofo buffer
            {
                free(new_item);
            }
        }
        else
        //it's empty
        {
            list_add_tail(&new_item->list,&tsk->rcv_ofo_buf);
        }
        seq_end = tsk->rcv_nxt;
        char new_data_recv =0;
        if(!list_empty(&tsk->rcv_ofo_buf))
        {
                list_for_each_entry_safe(item,q,&tsk->rcv_ofo_buf,list)
                {
                    if(seq_end == item->seq  && ring_buffer_free(tsk->rcv_buf) > (int)item->len)
                    {
                        new_data_recv =1;
                        seq_end = item->seq_end;
                        tsk->rcv_nxt = seq_end;// update recv info.
                        list_delete_entry(&item->list);
                        write_ring_buffer(tsk->rcv_buf, item->payload,item->len);
                        tsk->rcv_wnd -=item->len;
                        //wake_up(tsk->wait_recv);
                        //free(item);
                    }
                    else{
                        break;
                    }
                }
        }

        if(new_data_recv)
        {
                wake_up(tsk->wait_recv);
        }
        //tsk->adv_wnd = cb->rwnd;
        //update snd_wnd
        tcp_update_window_safe(tsk, cb);



        //reply with TCP_ACK if the connection is alive
        //if(tsk->state!=TCP_LAST_ACK)
        //{
         //   tsk->rcv_nxt = cb->seq_end;
          //  tcp_send_control_packet(tsk, TCP_ACK);
        //}
        printf("send ack.seq=%d,ack=%d\n",tsk->snd_nxt,tsk->rcv_nxt);
        tcp_send_control_packet(tsk, TCP_ACK);
    }

}
