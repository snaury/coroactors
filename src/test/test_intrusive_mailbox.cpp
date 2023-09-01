#include <coroactors/detail/intrusive_mailbox.h>
#include <gtest/gtest.h>

using namespace coroactors;

struct node_t : public detail::intrusive_mailbox_node<node_t> {
    int value;

    explicit node_t(int value) : value(value) {}
};

using mailbox_t = detail::intrusive_mailbox<node_t>;

TEST(TestIntrusiveMailbox, Basics) {
    mailbox_t m;
    node_t a(1);
    node_t b(2);
    node_t c(3);

    // It is initially locked
    ASSERT_TRUE(m.empty());
    ASSERT_FALSE(m.push(&a));
    ASSERT_FALSE(m.push(&b));
    ASSERT_FALSE(m.empty());
    ASSERT_EQ(m.pop(), &a);
    ASSERT_FALSE(m.empty());
    ASSERT_EQ(m.pop(), &b);
    ASSERT_TRUE(m.empty());
    ASSERT_EQ(m.pop(), nullptr);

    // It is unlocked, now push will return true
    ASSERT_TRUE(m.push(&c));
    ASSERT_EQ(m.peek(), &c);
    ASSERT_EQ(m.peek(), &c);
    ASSERT_FALSE(m.try_unlock());
    ASSERT_EQ(m.pop(), &c);
    ASSERT_EQ(m.peek(), nullptr);
    ASSERT_TRUE(m.try_unlock());
}

TEST(TestIntrusiveMailbox, TryLockUnlock) {
    mailbox_t m;
    node_t a(1);
    // It is initially already locked
    ASSERT_FALSE(m.try_lock()) << "unexpected lock of initially locked mailbox";
    ASSERT_TRUE(m.try_unlock()) << "expected to unlock an empty mailbox";
    ASSERT_TRUE(m.try_lock()) << "expected to lock an unlocked mailbox";
    ASSERT_FALSE(m.push(&a)) << "unexpected lock when pushing to a locked mailbox";
    ASSERT_FALSE(m.try_lock()) << "unexpected lock of a locked mailbox";
    ASSERT_FALSE(m.try_unlock()) << "unexpected unlock of a non-empty mailbox";
    ASSERT_EQ(m.pop(), &a);
    ASSERT_FALSE(m.try_lock()) << "unexpected lock of a locked mailbox";
    ASSERT_TRUE(m.try_unlock()) << "expected to unlock an empty mailbox";
    ASSERT_TRUE(m.try_lock()) << "expected to lock an unlocked mailbox";
}

TEST(TestIntrusiveMailbox, InitiallyUnlocked) {
    mailbox_t m(mailbox_t::initially_unlocked);
    node_t a(1);
    ASSERT_TRUE(m.push(&a));
    ASSERT_EQ(m.peek(), &a);
    ASSERT_EQ(m.pop(), &a);
    ASSERT_EQ(m.pop(), nullptr);
}

TEST(TestIntrusiveMailbox, MoveBetweenMailboxes) {
    mailbox_t m1(mailbox_t::initially_locked);
    mailbox_t m2(mailbox_t::initially_locked);
    node_t a(1);
    node_t b(2);
    node_t c(3);
    ASSERT_FALSE(m1.push(&a));
    ASSERT_FALSE(m1.push(&b));
    ASSERT_FALSE(m1.push(&c));
    ASSERT_EQ(m1.pop(), &a);
    ASSERT_EQ(m1.pop(), &b);
    ASSERT_EQ(m1.pop(), &c);
    ASSERT_FALSE(m2.push(&c));
    ASSERT_FALSE(m2.push(&b));
    ASSERT_FALSE(m2.push(&a));
    ASSERT_EQ(m2.pop(), &c);
    ASSERT_EQ(m2.pop(), &b);
    ASSERT_EQ(m2.pop(), &a);
}

TEST(TestIntrusiveMailbox, TryPop) {
    mailbox_t m(mailbox_t::initially_locked);
    node_t a(1);
    node_t b(2);
    ASSERT_FALSE(m.push(&a));
    ASSERT_FALSE(m.push(&b));
    ASSERT_EQ(m.try_pop(), &a);
    ASSERT_EQ(m.try_pop(), &b);
    ASSERT_EQ(m.try_pop(), nullptr);
    // Note: still locked!
    ASSERT_FALSE(m.push(&a));
    ASSERT_EQ(m.try_pop(), &a);
}

TEST(TestIntrusiveMailbox, UnpublishedPush) {
    mailbox_t m(mailbox_t::initially_locked);
    node_t a(1);
    node_t b(2);
    node_t c(3);
    ASSERT_FALSE(m.push(&a));
    // Start pushing b
    auto* b_prev = m.push_prepare(&b);
    ASSERT_EQ(b_prev, &a);
    // Node cannot be removed right now
    ASSERT_EQ(m.try_pop(), nullptr);
    // But we can see the current head
    ASSERT_EQ(m.peek(), &a);
    // Same for a pop, it unlocks
    ASSERT_EQ(m.pop(), nullptr);
    // Note the push of c will not lock
    ASSERT_FALSE(m.push(&c));
    // When push of b publishes it locks
    ASSERT_TRUE(m.push_publish(b_prev, &b));
    // Now we can finally remove the first item
    ASSERT_EQ(m.pop(), &a);
}
