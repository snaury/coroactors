#include <coroactors/detail/mailbox.h>
#include <gtest/gtest.h>

using namespace coroactors;

using mailbox_t = detail::mailbox<int>;

TEST(TestMailbox, Basics) {
    mailbox_t m;
    EXPECT_EQ(m.peek(), nullptr);
    EXPECT_FALSE(m.push(1));
    EXPECT_FALSE(m.push(2));
    ASSERT_FALSE(m.try_unlock());
    ASSERT_EQ(m.pop_default(), 1);
    ASSERT_EQ(m.pop_default(), 2);
    ASSERT_EQ(m.pop_default(), 0);
    ASSERT_TRUE(m.push(3));
    const int* current = m.peek();
    ASSERT_TRUE(current);
    ASSERT_EQ(*current, 3);
    ASSERT_FALSE(m.try_unlock());
    ASSERT_EQ(m.pop_default(), 3);
    ASSERT_TRUE(m.try_unlock());
}

TEST(TestMailbox, PushToInitiallyUnlocked) {
    mailbox_t m(mailbox_t::initially_unlocked);
    ASSERT_TRUE(m.push(1));
    EXPECT_FALSE(m.push(2));
    ASSERT_EQ(m.pop_default(), 1);
    ASSERT_EQ(m.pop_default(), 2);
    ASSERT_EQ(m.pop_default(), 0);
}
